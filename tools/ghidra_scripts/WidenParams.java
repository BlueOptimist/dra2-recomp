// Widens parameters typed narrower than the 4-byte stack slot the code reads. The decompiler sometimes types a
// parameter by one use ("short param_2") while another reads the whole slot through its address,
// *(undefined4 *)&param_2, and shows the upper half as in_stack_00000006. Recompiled, callers pass only the narrow
// value: a string pointer passed that way lost its upper half, so "%s/fx/fx_cam%.2d" printed from garbage.
// For each row of analysis/narrow_params.tsv (address, name, parameter, type; tools/find_narrow_params.py) the
// parameter becomes undefined4 in the same slot.
// Args: <narrow_params.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.symbol.SourceType;

public class WidenParams extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int widened = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				Parameter target = null;
				if (f != null) {
					for (Parameter p : f.getParameters()) {
						if (p.getName().equals(c[2])) {
							target = p;
						}
					}
				}
				if (target == null) {
					printerr("no parameter " + c[2] + " in " + c[0]);
					failed++;
					continue;
				}
				try {
					target.setDataType(Undefined4DataType.dataType, SourceType.USER_DEFINED);
					widened++;
				}
				catch (Exception e) {
					printerr(c[0] + " " + c[2] + ": " + e.getMessage());
					failed++;
				}
			}
		}
		println(String.format("parameters widened to 4 bytes: %d, failed: %d", widened, failed));
	}
}
