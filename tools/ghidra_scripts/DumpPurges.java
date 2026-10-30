// Prints the calling convention, stack purge and parameter storage Ghidra holds for the given functions (the
// exported C doesn't show the purge, and a wrong one shifts the stack offsets in every caller).
// Args: <hex address> [<hex address> ...]
// @category Digimon
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;

public class DumpPurges extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String a : getScriptArgs()) {
			Function f = getFunctionAt(toAddr(Long.parseLong(a, 16)));
			if (f == null) {
				println(a + ": no function");
				continue;
			}
			StringBuilder sb = new StringBuilder();
			for (Parameter p : f.getParameters()) {
				sb.append(" ").append(p.getName()).append("@").append(p.getVariableStorage());
			}
			println(String.format("%s %s cc=%s purge=%d custom=%b params:%s", a, f.getName(),
				f.getCallingConventionName(), f.getStackPurgeSize(), f.hasCustomVariableStorage(), sb));
		}
	}
}
