// Dumps matching features of every function as JSON lines: entry, qualified name, size, instruction count,
// ordered call targets, referenced strings and large scalar constants. Used by tools/match-builds.mjs to carry the
// Linux build's symbol names over to GSystem.dll.
// Script args: <out.jsonl>
//@category CiM

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class DumpFeatures extends GhidraScript {

	private static String esc(String s) {
		StringBuilder b = new StringBuilder("\"");
		for (char c : s.toCharArray()) {
			if (c == '"' || c == '\\') b.append('\\').append(c);
			else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
			else b.append(c);
		}
		return b.append('"').toString();
	}

	@Override
	protected void run() throws Exception {
		Path out = Paths.get(getScriptArgs()[0]);
		Listing listing = currentProgram.getListing();
		ReferenceManager refs = currentProgram.getReferenceManager();
		int n = 0;
		try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out, StandardCharsets.UTF_8))) {
			for (Function f : listing.getFunctions(true)) {
				if (monitor.isCancelled()) break;
				List<String> calls = new ArrayList<>();
				Set<String> strings = new LinkedHashSet<>();
				Set<Long> consts = new TreeSet<>();
				int insns = 0;
				for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
					insns++;
					if (ins.getFlowType().isCall()) {
						for (Address t : ins.getFlows()) calls.add("0x" + t.toString().toLowerCase());
					}
					for (Reference r : refs.getReferencesFrom(ins.getAddress())) {
						Data d = listing.getDataAt(r.getToAddress());
						if (d != null && d.hasStringValue()) {
							Object v = d.getValue();
							if (v != null && v.toString().length() >= 3) strings.add(v.toString());
						}
					}
					for (int i = 0; i < ins.getNumOperands(); i++) {
						for (Object o : ins.getOpObjects(i)) {
							if (o instanceof Scalar) {
								long v = ((Scalar) o).getUnsignedValue();
								if (v > 0xFFFF && v < 0xFFFF0000L && !(v >= 0x08000000L && v < 0x0A000000L) && !(v >= 0x10000000L && v < 0x10200000L)) consts.add(v);
							}
						}
					}
				}
				StringBuilder b = new StringBuilder();
				b.append("{\"entry\":\"0x").append(f.getEntryPoint().toString().toLowerCase()).append('"');
				b.append(",\"name\":").append(esc(f.getName(true)));
				b.append(",\"size\":").append(f.getBody().getNumAddresses());
				b.append(",\"insns\":").append(insns);
				b.append(",\"calls\":[");
				for (int i = 0; i < calls.size(); i++) b.append(i > 0 ? "," : "").append('"').append(calls.get(i)).append('"');
				b.append("],\"strings\":[");
				int i = 0;
				for (String s : strings) b.append(i++ > 0 ? "," : "").append(esc(s));
				b.append("],\"consts\":[");
				i = 0;
				for (Long c : consts) b.append(i++ > 0 ? "," : "").append(c);
				b.append("]}");
				w.println(b);
				n++;
			}
		}
		println("DumpFeatures: " + n + " functions -> " + out);
	}
}
