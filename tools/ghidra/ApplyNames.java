// Names GSystem.dll functions after their Linux build counterparts (decompiled/GSystem-v2/linux-names.csv, written
// by tools/match-builds.mjs). "gsClock::move" becomes function "move" in class namespace "gsClock". Only functions
// that still carry a default name (FUN_...) are renamed; a function that already has another name (library, export)
// keeps it. Every matched function gets a comment with the Linux name, address and how the match was found.
// Script args: <linux-names.csv>
//@category CiM

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class ApplyNames extends GhidraScript {

	// CSV line -> fields; a field may be quoted with "" escapes
	private static List<String> csv(String line) {
		List<String> out = new ArrayList<>();
		StringBuilder b = new StringBuilder();
		boolean q = false;
		for (int i = 0; i < line.length(); i++) {
			char c = line.charAt(i);
			if (q) {
				if (c == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') { b.append('"'); i++; }
				else if (c == '"') q = false;
				else b.append(c);
			}
			else if (c == '"') q = true;
			else if (c == ',') { out.add(b.toString()); b.setLength(0); }
			else b.append(c);
		}
		out.add(b.toString());
		return out;
	}

	// "a::b<c::d>::f" -> [a, b<c::d>, f]: splits on "::" outside template and parameter brackets
	private static List<String> parts(String name) {
		List<String> out = new ArrayList<>();
		int depth = 0, start = 0;
		for (int i = 0; i < name.length(); i++) {
			char c = name.charAt(i);
			if (c == '<' || c == '(') depth++;
			else if ((c == '>' || c == ')') && depth > 0) depth--;
			else if (depth == 0 && c == ':' && i + 1 < name.length() && name.charAt(i + 1) == ':') {
				out.add(name.substring(start, i));
				start = i + 2;
				i++;
			}
		}
		out.add(name.substring(start));
		return out;
	}

	@Override
	protected void run() throws Exception {
		List<String> lines = Files.readAllLines(Paths.get(getScriptArgs()[0]), StandardCharsets.UTF_8);
		SymbolTable st = currentProgram.getSymbolTable();
		FunctionManager fm = currentProgram.getFunctionManager();
		int renamed = 0, kept = 0, missing = 0, failed = 0;
		for (String line : lines.subList(1, lines.size())) {
			if (line.isBlank()) continue;
			List<String> f = csv(line);
			String win = f.get(0), lin = f.get(1), name = f.get(2), method = f.get(3);
			Function fn = fm.getFunctionAt(toAddr(win));
			if (fn == null) { missing++; continue; }
			String note = "Linux build: " + name + " @ " + lin + " (matched by " + method + ")";
			String old = fn.getComment();
			if (old == null || !old.contains("Linux build:")) fn.setComment(old == null ? note : old + "\n" + note);
			if (fn.getSymbol().getSource() != SourceType.DEFAULT && fn.getSymbol().getSource() != SourceType.ANALYSIS) {
				kept++;
				continue;
			}
			try {
				List<String> p = parts(name);
				Namespace ns = currentProgram.getGlobalNamespace();
				for (String n : p.subList(0, p.size() - 1)) {
					n = SymbolUtilities.replaceInvalidChars(n, true);
					Namespace next = st.getNamespace(n, ns);
					ns = next != null ? next : st.createClass(ns, n, SourceType.IMPORTED);
				}
				fn.setParentNamespace(ns);
				fn.setName(SymbolUtilities.replaceInvalidChars(p.get(p.size() - 1), true), SourceType.IMPORTED);
				renamed++;
			}
			catch (Exception e) {
				failed++;
				println("failed " + win + " " + name + ": " + e.getMessage());
			}
		}
		println("ApplyNames: renamed " + renamed + ", kept existing name " + kept + ", no function " + missing + ", failed " + failed);
	}
}
