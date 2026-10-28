// Applies function names (and evidence as plate comments) from a TSV: address, name, evidence.
// Args: <function_names.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int applied = 0, missing = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				if (line.isBlank()) {
					continue;
				}
				String[] c = line.split("\t", 3);
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					f = createFunction(toAddr(Long.parseLong(c[0], 16)), null);
				}
				if (f == null) {
					println("no function at " + c[0]);
					missing++;
					continue;
				}
				f.setName(c[1], SourceType.USER_DEFINED);
				if (c.length > 2) {
					f.setComment(c[2]);
				}
				applied++;
			}
		}
		println(String.format("applied %d names, %d missing", applied, missing));
	}
}
