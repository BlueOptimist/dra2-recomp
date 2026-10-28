// Prints parameter storage for the given functions and counts class methods by storage kind.
// Args: <hex address> [...]
// @category Digimon
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Parameter;

public class InspectParams extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String a : getScriptArgs()) {
			Function f = getFunctionAt(toAddr(Long.parseLong(a, 16)));
			println(f.getName(true) + " conv=" + f.getCallingConventionName() + " custom=" + f.hasCustomVariableStorage() +
				" sigSource=" + f.getSignatureSource());
			for (Parameter p : f.getParameters()) {
				println("   " + p.getName() + " auto=" + p.isAutoParameter() + " storage=" + p.getVariableStorage() +
					" type=" + p.getDataType());
			}
		}
		int total = 0, custom = 0, noThis = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (!(f.getParentNamespace() instanceof GhidraClass)) {
				continue;
			}
			total++;
			if (f.hasCustomVariableStorage()) {
				custom++;
			}
			if (f.getParameterCount() == 0 || !f.getParameter(0).isAutoParameter()) {
				noThis++;
			}
		}
		println("class functions " + total + ", custom storage " + custom + ", without auto this " + noThis);
	}
}
