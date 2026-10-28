// Decompiles every function in the given memory block and writes C files grouped by module and address, plus a tracker.
// Args: <out_dir> <block name, e.g. .text> <functions per file> [<modules.tsv>]
// modules.tsv rows (start, end, module) route library code to lib/<module>/; everything else is game code in src/.
// @category Digimon
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ExportDecomp extends GhidraScript {
	private static final String GAME = "game";

	private static String moduleOf(long addr, List<long[]> ranges, List<String> names) {
		for (int i = 0; i < ranges.size(); i++) {
			if (addr >= ranges.get(i)[0] && addr < ranges.get(i)[1]) {
				return names.get(i);
			}
		}
		return GAME;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args[0]);
		MemoryBlock block = currentProgram.getMemory().getBlock(args[1]);
		int perFile = Integer.parseInt(args[2]);
		List<long[]> ranges = new ArrayList<>();
		List<String> rangeNames = new ArrayList<>();
		if (args.length > 3) {
			try (BufferedReader in = new BufferedReader(new FileReader(args[3]))) {
				in.readLine();
				for (String line; (line = in.readLine()) != null;) {
					String[] c = line.split("\t");
					if (c.length >= 3) {
						ranges.add(new long[] { Long.parseLong(c[0], 16), Long.parseLong(c[1], 16) });
						rangeNames.add(c[2]);
					}
				}
			}
		}

		// Game and engine code that ships is "src"; the rest is reference-only library code under lib/<module>.
		Map<String, List<Function>> byModule = new LinkedHashMap<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(block.getStart(), true)) {
			if (!block.contains(f.getEntryPoint())) {
				break;
			}
			if (!f.isThunk()) {
				String m = moduleOf(f.getEntryPoint().getOffset(), ranges, rangeNames);
				byModule.computeIfAbsent(m, k -> new ArrayList<>()).add(f);
			}
		}

		DecompInterface di = new DecompInterface();
		DecompileOptions opts = new DecompileOptions();
		di.setOptions(opts);
		di.openProgram(currentProgram);
		int ok = 0, failed = 0;
		try (PrintWriter tracker = new PrintWriter(new File(outDir, "functions.tsv"))) {
			tracker.println("address\tname\tsize\tmodule\tfile\tname_source\tstatus");
			for (Map.Entry<String, List<Function>> mod : byModule.entrySet()) {
			String module = mod.getKey();
			List<Function> funcs = mod.getValue();
			boolean isGame = module.equals(GAME) || module.equals("engine_core") || module.equals("static_init");
			String dir = isGame ? "src/" + (module.equals(GAME) ? "" : module + "/") : "lib/" + module + "/";
			new File(outDir, dir).mkdirs();
			for (int start = 0; start < funcs.size(); start += perFile) {
				List<Function> chunk = funcs.subList(start, Math.min(funcs.size(), start + perFile));
				String fileName = String.format("%stext_%08x.c", dir, chunk.get(0).getEntryPoint().getOffset());
				try (PrintWriter out = new PrintWriter(new File(outDir, fileName))) {
					out.println("// Raw Ghidra decompilation of default.xbe " + args[1] + " " + chunk.get(0).getEntryPoint() +
						" - " + chunk.get(chunk.size() - 1).getEntryPoint() + " [module: " + module + "]." +
						(isGame ? " Not yet compilable; see decomp/README.md."
								: " Statically linked library: reference only, replaced on Windows rather than ported."));
					out.println("#include \"" + (dir.chars().filter(ch -> ch == '/').count() > 1 ? "../" : "") + "../ghidra_types.h\"\n");
					for (Function f : chunk) {
						monitor.checkCancelled();
						DecompileResults r = di.decompileFunction(f, 60, monitor);
						String status;
						out.println("// " + f.getEntryPoint() + " " + f.getName(true) + " (" + f.getBody().getNumAddresses() + " bytes)");
						if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
							out.println(r.getDecompiledFunction().getC());
							status = "raw";
							ok++;
						}
						else {
							out.println("// DECOMPILE FAILED: " + r.getErrorMessage() + "\n");
							status = "failed";
							failed++;
						}
						SourceType src = f.getSymbol().getSource();
						tracker.println(f.getEntryPoint() + "\t" + f.getName(true) + "\t" + f.getBody().getNumAddresses() + "\t" +
							module + "\t" + fileName + "\t" + (f.getName().startsWith("FUN_") ? "none" : src.toString().toLowerCase()) +
							"\t" + (isGame ? status : "library"));
					}
				}
				println(String.format("%s: %d/%d functions", module, Math.min(funcs.size(), start + perFile), funcs.size()));
			}
			}
		}
		di.dispose();
		println(String.format("decompiled %d, failed %d", ok, failed));
	}
}
