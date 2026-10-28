// Finds instructions whose scalar operands equal .res chunk/Maya-type FourCC tags and reports the functions using them.
// Args: <report.tsv>
// @category Digimon
import java.io.PrintWriter;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindTagRefs extends GhidraScript {
	private static final String[] TAGS = { "res\n", "surf", "rdms", "banm", "wave", "gshd", "ndbg", "node", "indx",
		"strg", "tern", "bmsh", "body", "lua!", "levl", "tabl", "minf", "font", "musc", "mdat" };

	@Override
	protected void run() throws Exception {
		Map<Long, String> values = new TreeMap<>();
		for (String t : TAGS) {
			byte[] b = t.getBytes("latin1");
			long le = (b[0] & 0xFFL) | (b[1] & 0xFFL) << 8 | (b[2] & 0xFFL) << 16 | (b[3] & 0xFFL) << 24;
			long be = (b[3] & 0xFFL) | (b[2] & 0xFFL) << 8 | (b[1] & 0xFFL) << 16 | (b[0] & 0xFFL) << 24;
			values.put(le, t);
			values.put(be, t + "(BE)");
		}
		Map<Function, Set<String>> found = new TreeMap<>((a, c) -> a.getEntryPoint().compareTo(c.getEntryPoint()));
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction ins = it.next();
			for (int op = 0; op < ins.getNumOperands(); op++) {
				for (Object o : ins.getOpObjects(op)) {
					if (o instanceof Scalar) {
						String tag = values.get(((Scalar) o).getUnsignedValue() & 0xFFFFFFFFL);
						Function f = getFunctionContaining(ins.getAddress());
						if (tag != null && f != null) {
							found.computeIfAbsent(f, k -> new TreeSet<>()).add(tag);
						}
					}
				}
			}
		}
		try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
			out.println("function\tname\tsize\ttags");
			for (Map.Entry<Function, Set<String>> e : found.entrySet()) {
				Function f = e.getKey();
				out.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses() + "\t" +
					String.join(",", e.getValue()));
			}
		}
		println("functions referencing tags: " + found.size());
	}
}
