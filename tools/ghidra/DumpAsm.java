//Dump the instruction listing of functions (by address) with call targets and data constants, one <addr>.asm per
//function: run headless with -postScript DumpAsm.java <outDir> <addr>... (used for docs/renderer/shadows.md).
//@category CiM
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DumpAsm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(args[i]);
            Function f = getFunctionAt(a);
            if (f == null) f = getFunctionContaining(a);
            PrintWriter pw = new PrintWriter(new FileWriter(new File(outDir, args[i] + ".asm")));
            pw.println("; " + (f != null ? f.getName(true) + " | " + f.getSignature() : "?"));
            AddressSetView body = f != null ? f.getBody() : new AddressSet(a, a.add(0x200));
            InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                StringBuilder sb = new StringBuilder();
                sb.append(ins.getAddress()).append("  ").append(ins.toString());
                for (Reference r : ins.getReferencesFrom()) {
                    Address to = r.getToAddress();
                    if (!to.isMemoryAddress()) continue;
                    Symbol s = getSymbolAt(to);
                    if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) {
                        Function cf = getFunctionAt(to);
                        if (cf != null) sb.append("    ; -> ").append(cf.getName(true));
                        else if (s != null && r.getReferenceType().isCall()) sb.append("    ; -> ").append(s.getName(true));
                    } else {
                        try {
                            int v = getInt(to);
                            float fv = Float.intBitsToFloat(v);
                            sb.append("    ; [").append(to).append("] ");
                            if (s != null) sb.append(s.getName(true)).append(" ");
                            sb.append(String.format("0x%08x f=%.9g", v, fv));
                            long lv = getLong(to);
                            double dv = Double.longBitsToDouble(lv);
                            sb.append(String.format(" d=%.12g", dv));
                        } catch (Exception e) {
                        }
                    }
                }
                pw.println(sb);
            }
            pw.close();
        }
    }
}
