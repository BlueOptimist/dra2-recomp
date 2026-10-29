// Repairs signatures that were committed before their parameters were. Marking a function's signature user-defined
// when Ghidra had only inferred its parameters freezes it with none ("Unknown calling convention -- yet parameter
// storage is locked"), and every caller then passes nothing and gets void back.
// For each address given: unfreeze the signature, decompile, store the inferred parameters, convention and return
// in the database (user-defined), and give a void function on analysis/lost_returns.tsv its return type.
// Args: <addresses.txt (one hex address per line)> <lost_returns.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.HashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Float10DataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.symbol.SourceType;

public class RepairSignatures extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Map<Long, String> lostReturns = new HashMap<>();
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[1]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				lostReturns.put(Long.parseLong(c[0], 16), c[2]);
			}
		}
		DecompInterface decomp = new DecompInterface();
		decomp.openProgram(currentProgram);
		int repaired = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			for (String line; (line = in.readLine()) != null;) {
				line = line.trim();
				if (line.isEmpty()) {
					continue;
				}
				long addr = Long.parseLong(line, 16);
				Function f = getFunctionAt(toAddr(addr));
				if (f == null) {
					failed++;
					continue;
				}
				f.setSignatureSource(SourceType.DEFAULT);
				f.setCustomVariableStorage(false);
				f.setCallingConvention("unknown");
				DecompileResults r = decomp.decompileFunction(f, 60, monitor);
				HighFunction hf = r.getHighFunction();
				if (hf == null) {
					printerr("could not decompile " + f.getName());
					failed++;
					continue;
				}
				HighFunctionDBUtil.commitParamsToDatabase(hf, true, HighFunctionDBUtil.ReturnCommitOption.COMMIT,
					SourceType.USER_DEFINED);
				String lost = lostReturns.get(addr);
				if (lost != null && f.getReturnType() instanceof VoidDataType) {
					DataType rt = lost.equals("float10") ? Float10DataType.dataType : Undefined4DataType.dataType;
					f.setReturnType(rt, SourceType.USER_DEFINED);
				}
				f.setSignatureSource(SourceType.USER_DEFINED);
				println(f.getSignature().getPrototypeString() + " [" + f.getCallingConventionName() + "]");
				repaired++;
			}
		}
		decomp.dispose();
		println(String.format("signatures repaired: %d, failed: %d", repaired, failed));
	}
}
