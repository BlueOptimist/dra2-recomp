// Dumps the direct call graph: caller \t callee (function entry addresses).
// Args: <calls.tsv>
// @category Digimon
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DumpCalls extends GhidraScript {
	@Override
	protected void run() throws Exception {
		try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
			out.println("caller\tcallee");
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				for (Function c : f.getCalledFunctions(monitor)) {
					out.println(f.getEntryPoint() + "\t" + c.getEntryPoint());
				}
			}
		}
	}
}
