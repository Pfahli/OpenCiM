// Post-analysis for GSystem.dll (Cities in Motion):
//   0. apply Ghidra's PEFunctionsThatDoNotReturn list with normalized names (FID names such as
//      "__CxxThrowException@8" do not match the list's "_CxxThrowException" otherwise),
//   1. report functions still marked non-returning,
//   2. define functions at code addresses referenced from data that auto-analysis left undefined
//      (list produced by tools/codeptrs.mjs), then re-run analysis on the changes,
//   2b. define functions at code addresses referenced only by instruction operands (native handlers registered
//      with "push offset FUN", callbacks), repeated to a fixpoint,
//   3. decompile every function in parallel into <out>/functions/0x<addr>.c and write <out>/index.csv.
//
// Script args: <candidateList.txt> <outDir> [threads=4] [timeoutSecs=60]
//@category CiM

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

import generic.jar.ResourceFile;
import ghidra.app.decompiler.*;
import ghidra.framework.Application;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.TaskMonitor;

public class RecoverAndDecompile extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		Path candidates = Paths.get(args[0]);
		Path out = Paths.get(args[1]);
		int threads = args.length > 2 ? Integer.parseInt(args[2]) : 4;
		int timeout = args.length > 3 ? Integer.parseInt(args[3]) : 60;
		Files.createDirectories(out.resolve("functions"));

		FunctionManager fm = currentProgram.getFunctionManager();
		int before = fm.getFunctionCount();

		// 0. Known non-returning library functions, matched without leading '_' and '@N' suffixes
		Set<String> known = new HashSet<>();
		ResourceFile list = Application.getModuleDataFile("Base", "PEFunctionsThatDoNotReturn");
		for (String line : Files.readAllLines(list.getFile(false).toPath())) {
			line = line.trim();
			if (!line.isEmpty() && !line.startsWith("#")) {
				known.add(normalize(line));
			}
		}
		for (Function f : fm.getFunctions(true)) {
			if (!f.hasNoReturn() && known.contains(normalize(f.getName()))) {
				f.setNoReturn(true);
				println("marked non-returning: " + f.getName() + " @ " + f.getEntryPoint());
			}
		}

		// 1. Non-returning functions after analysis
		try (PrintWriter w = writer(out.resolve("noreturn.txt"))) {
			for (Function f : fm.getFunctions(true)) {
				if (f.hasNoReturn()) {
					w.println(f.getEntryPoint() + " " + f.getName());
				}
			}
		}

		// 2. Recover functions at referenced-but-undefined code addresses
		Set<Address> created = new HashSet<>();
		Map<String, Integer> tally = new TreeMap<>();
		try (PrintWriter w = writer(out.resolve("recovery.csv"))) {
			w.println("address,status,function");
			for (String line : Files.readAllLines(candidates)) {
				line = line.trim();
				if (line.isEmpty()) {
					continue;
				}
				Address a = toAddr(line);
				String status;
				Function f = getFunctionContaining(a);
				if (f != null) {
					status = f.getEntryPoint().equals(a) ? "entry" : "inside";
				}
				else if (currentProgram.getListing().getDefinedDataContaining(a) != null) {
					status = "data";
				}
				else {
					if (getInstructionAt(a) == null) {
						disassemble(a);
					}
					if (getInstructionAt(a) == null) {
						status = "not-code";
					}
					else {
						f = createFunction(a, null);
						if (f != null) {
							created.add(a);
							status = "created";
						}
						else {
							status = "create-failed";
						}
					}
				}
				tally.merge(status, 1, Integer::sum);
				w.println(line + "," + status + "," + (f == null ? "" : f.getName()));
			}
		}
		println("Recovery: " + tally);
		analyzeChanges(currentProgram);

		// 2b. Functions referenced only by instruction operands (e.g. "push offset handler" when registering native
		//     script functions): iterate to a fixpoint, since new functions expose further references.
		MemoryBlock text = currentProgram.getMemory().getBlock(".text");
		try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("recovery.csv"),
			StandardCharsets.UTF_8, StandardOpenOption.APPEND))) {
			for (int round = 1; round <= 6; round++) {
				Set<Address> targets = new TreeSet<>();
				InstructionIterator it = currentProgram.getListing().getInstructions(true);
				while (it.hasNext()) {
					Instruction ins = it.next();
					for (Reference r : ins.getReferencesFrom()) {
						addTarget(targets, text, r.getToAddress());
					}
					for (int i = 0; i < ins.getNumOperands(); i++) {
						for (Object o : ins.getOpObjects(i)) {
							if (o instanceof Scalar s && s.bitLength() == 32) {
								long v = s.getUnsignedValue();
								if (v >= text.getStart().getOffset() && v <= text.getEnd().getOffset()) {
									addTarget(targets, text, toAddr(v));
								}
							}
						}
					}
				}
				int made = 0;
				for (Address a : targets) {
					if (getFunctionContaining(a) != null) {
						continue;
					}
					if (getInstructionAt(a) == null && getInstructionContaining(a) == null) {
						disassemble(a);
					}
					if (getInstructionAt(a) == null) {
						continue;
					}
					Function f = createFunction(a, null);
					if (f != null) {
						created.add(a);
						made++;
						w.println("0x" + a + ",created-operand-ref," + f.getName());
					}
				}
				println("Operand-reference recovery round " + round + ": " + made + " functions");
				if (made == 0) {
					break;
				}
				analyzeChanges(currentProgram);
			}
		}
		int after = fm.getFunctionCount();
		println("Functions before recovery: " + before + ", after: " + after);

		// 3. Parallel decompilation of every non-external function
		List<Function> all = new ArrayList<>();
		for (Function f : fm.getFunctions(true)) {
			if (!f.isExternal()) {
				all.add(f);
			}
		}
		ConcurrentLinkedQueue<String> rows = new ConcurrentLinkedQueue<>();
		AtomicInteger done = new AtomicInteger();
		AtomicInteger failed = new AtomicInteger();
		ExecutorService pool = Executors.newFixedThreadPool(threads);
		ThreadLocal<DecompInterface> ifc = ThreadLocal.withInitial(() -> {
			DecompInterface d = new DecompInterface();
			d.setOptions(new DecompileOptions());
			d.toggleCCode(true);
			d.toggleSyntaxTree(false);
			d.setSimplificationStyle("decompile");
			d.openProgram(currentProgram);
			return d;
		});
		List<DecompInterface> opened = Collections.synchronizedList(new ArrayList<>());
		List<Future<?>> futures = new ArrayList<>();
		for (Function f : all) {
			futures.add(pool.submit(() -> {
				DecompInterface d = ifc.get();
				if (!opened.contains(d)) {
					opened.add(d);
				}
				decompileOne(d, f, out, timeout, created, rows, failed);
				int n = done.incrementAndGet();
				if (n % 1000 == 0) {
					println("decompiled " + n + "/" + all.size());
				}
			}));
		}
		for (Future<?> fu : futures) {
			fu.get();
		}
		pool.shutdown();
		for (DecompInterface d : opened) {
			d.dispose();
		}

		List<String> sorted = new ArrayList<>(rows);
		Collections.sort(sorted);
		try (PrintWriter w = writer(out.resolve("index.csv"))) {
			w.println("address,name,body_bytes,recovered,noreturn,decompiled,noreturn_warning");
			for (String r : sorted) {
				w.println(r);
			}
		}
		println("Decompiled " + (all.size() - failed.get()) + "/" + all.size() + " functions; failures: " + failed.get());
	}

	private void decompileOne(DecompInterface d, Function f, Path out, int timeout, Set<Address> created,
			Queue<String> rows, AtomicInteger failed) {
		String addr = "0x" + f.getEntryPoint().toString().toLowerCase();
		StringBuilder hdr = new StringBuilder();
		// qualified name (Class::method for demangled C++ symbols, e.g. the Linux build) and the prototype
		String qname = f.getName(true);
		hdr.append("// name: ").append(qname).append('\n');
		hdr.append("// signature: ").append(f.getPrototypeString(false, false)).append('\n');
		hdr.append("// address: ").append(addr).append('\n');
		for (AddressRange r : f.getBody().getAddressRanges()) {
			hdr.append("// range: 0x").append(r.getMinAddress()).append("-0x").append(r.getMaxAddress()).append('\n');
		}
		boolean recovered = created.contains(f.getEntryPoint());
		hdr.append("// recovered: ").append(recovered).append('\n');
		hdr.append("// noreturn: ").append(f.hasNoReturn()).append('\n');
		hdr.append("// pseudocode: Ghidra 12.1.4 decompiler output (Non-Returning Functions - Discovered disabled); not original source\n");
		String c;
		boolean ok;
		try {
			DecompileResults res = d.decompileFunction(f, timeout, TaskMonitor.DUMMY);
			ok = res != null && res.decompileCompleted() && res.getDecompiledFunction() != null;
			c = ok ? res.getDecompiledFunction().getC() : "// decompile failed: " + (res == null ? "null result" : res.getErrorMessage()) + "\n";
		}
		catch (Exception e) {
			ok = false;
			c = "// decompile failed: " + e + "\n";
		}
		if (!ok) {
			failed.incrementAndGet();
		}
		boolean warn = c.contains("Subroutine does not return");
		try {
			Files.write(out.resolve("functions").resolve(addr + ".c"), (hdr + c).getBytes(StandardCharsets.UTF_8));
		}
		catch (IOException e) {
			throw new UncheckedIOException(e);
		}
		String csvName = qname.contains(",") || qname.contains("\"") ? "\"" + qname.replace("\"", "\"\"") + "\"" : qname;
		rows.add(addr + "," + csvName + "," + f.getBody().getNumAddresses() + "," + (recovered ? 1 : 0) + ","
			+ (f.hasNoReturn() ? 1 : 0) + "," + (ok ? 1 : 0) + "," + (warn ? 1 : 0));
	}

	private void addTarget(Set<Address> targets, MemoryBlock text, Address to) {
		if (to == null || !text.contains(to) || getFunctionContaining(to) != null) {
			return;
		}
		if (currentProgram.getListing().getDefinedDataContaining(to) != null) {
			return;
		}
		// skip addresses in the middle of an existing instruction (scalar constants that merely look like addresses)
		Instruction in = getInstructionContaining(to);
		if (in != null && !in.getAddress().equals(to)) {
			return;
		}
		targets.add(to);
	}

	private static String normalize(String name) {
		return name.replaceAll("^_+", "").replaceAll("@\\d+$", "");
	}

	private static PrintWriter writer(Path p) throws IOException {
		return new PrintWriter(Files.newBufferedWriter(p, StandardCharsets.UTF_8));
	}
}
