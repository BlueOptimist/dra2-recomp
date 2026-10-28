// Summarizes function counts per memory block and how many got real (non-FUN_) names.
// @category Digimon
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;

public class FunctionStats extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Map<String, int[]> perBlock = new TreeMap<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (f.isThunk() || f.isExternal()) {
				continue;
			}
			MemoryBlock block = currentProgram.getMemory().getBlock(f.getEntryPoint());
			String name = block == null ? "?" : block.getName();
			int[] counts = perBlock.computeIfAbsent(name, k -> new int[2]);
			counts[0]++;
			if (!f.getName().startsWith("FUN_")) {
				counts[1]++;
			}
		}
		int total = 0, named = 0;
		for (Map.Entry<String, int[]> e : perBlock.entrySet()) {
			println(String.format("%-10s %6d functions, %6d named", e.getKey(), e.getValue()[0], e.getValue()[1]));
			total += e.getValue()[0];
			named += e.getValue()[1];
		}
		println(String.format("%-10s %6d functions, %6d named", "TOTAL", total, named));
	}
}
