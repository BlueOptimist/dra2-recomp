// Gives overloaded functions distinct names. Ghidra allows two functions with the same name in one namespace
// (e.g. two constructors Class_2ae928::ctor taking different arguments); C doesn't, and a call site naming "ctor"
// can't say which one it means, so recompiled code called the wrong overload. Within each namespace, the
// lowest-address function keeps the name and every other one gets its address appended (ctor_001ccb30); call sites
// in the decompiled output then name the exact function.
// @category Digimon
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class RenameOverloads extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Map<String, List<Function>> byName = new HashMap<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (f.isThunk() || f.isExternal()) {
				continue;
			}
			byName.computeIfAbsent(f.getName(true), k -> new ArrayList<>()).add(f);
		}
		int renamed = 0, groups = 0;
		for (List<Function> fs : byName.values()) {
			if (fs.size() < 2) {
				continue;
			}
			groups++;
			fs.sort((a, b) -> a.getEntryPoint().compareTo(b.getEntryPoint()));
			for (Function f : fs.subList(1, fs.size())) {
				String name = f.getName() + "_" + String.format("%08x", f.getEntryPoint().getOffset());
				f.setName(name, SourceType.USER_DEFINED);
				renamed++;
			}
		}
		println("overloaded names: " + groups + ", functions renamed: " + renamed);
	}
}
