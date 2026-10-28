// Decompiles the functions at the given addresses and writes their C to a file.
// Args: <out.c> <hex address> [<hex address> ...]
// @category Digimon
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileList extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		try (PrintWriter out = new PrintWriter(args[0])) {
			for (int i = 1; i < args.length; i++) {
				Function f = getFunctionAt(toAddr(Long.parseLong(args[i], 16)));
				if (f == null) {
					out.println("// no function at " + args[i]);
					continue;
				}
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
				out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
			}
		}
		di.dispose();
	}
}
