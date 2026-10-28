// Restores return values the committed signatures dropped. When a function is typed void but its callers use the
// register it leaves its result in, the decompiler shows that as extraout_EAX / extraout_ST0 after the call; once
// recompiled those are uninitialised locals, so the result is lost (an allocator that "fails", ...).
// Reads analysis/lost_returns.tsv (address, name, return, uses: from tools' scan of the exported source) and gives
// every function there that is still void the listed return type (undefined4 in EAX, float10 in ST0).
// Args: <lost_returns.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Float10DataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class SetLostReturns extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int changed = 0, kept = 0, missing = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					missing++;
					continue;
				}
				if (f.isThunk()) {
					f = f.getThunkedFunction(true);
				}
				if (!(f.getReturnType() instanceof VoidDataType) || f.hasCustomVariableStorage()) {
					kept++;
					continue;
				}
				DataType rt = c[2].equals("float10") ? Float10DataType.dataType : Undefined4DataType.dataType;
				f.setReturnType(rt, SourceType.USER_DEFINED);
				changed++;
			}
		}
		println(String.format("return types restored: %d; already non-void or custom: %d; no function: %d", changed,
			kept, missing));
	}
}
