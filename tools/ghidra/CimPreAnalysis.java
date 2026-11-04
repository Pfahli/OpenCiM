// Pre-analysis settings for GSystem.dll (Cities in Motion).
//
// Ghidra's "Non-Returning Functions - Discovered" analyzer marks MSVC operator delete (FUN_10087080) and the string
// destructors that call it as non-returning, which truncates ~27% of all function bodies. Known non-returning library
// functions (e.g. _CxxThrowException) are still handled by the "Known" analyzer.
//@category CiM

import ghidra.app.script.GhidraScript;

public class CimPreAnalysis extends GhidraScript {
	@Override
	public void run() throws Exception {
		setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
		println("CimPreAnalysis: disabled 'Non-Returning Functions - Discovered'");
	}
}
