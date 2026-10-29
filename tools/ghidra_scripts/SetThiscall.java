// Makes functions that read ECX as an input __thiscall. Ghidra typed them with a stack-only convention, so the body
// shows the object pointer as in_ECX and the call sites don't pass it; recompiled, in_ECX is an uninitialised local
// (e.g. a constructor of a global object writing through garbage). As __thiscall, ECX becomes the automatic `this`
// parameter, and the decompiler shows the value each caller loads into ECX.
// Reads analysis/ecx_this.tsv (one address per line, after a header): stack-convention functions whose only register
// input is ECX, from a scan of the exported source.
// Args: <ecx_this.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class SetThiscall extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int changed = 0, skipped = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				if (line.isBlank()) {
					continue;
				}
				Function f = getFunctionAt(toAddr(Long.parseLong(line.trim(), 16)));
				if (f == null || f.hasCustomVariableStorage() || "__thiscall".equals(f.getCallingConventionName())) {
					skipped++;
					continue;
				}
				f.setCallingConvention("__thiscall");
				changed++;
			}
		}
		println("made __thiscall: " + changed + ", skipped: " + skipped);
	}
}
