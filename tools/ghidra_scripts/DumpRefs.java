// Dumps (1) the strings each function references and (2) {string pointer, function pointer} tables in data sections.
// Args: <function_strings.tsv> <name_tables.tsv>
// @category Digimon
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class DumpRefs extends GhidraScript {
	// XBE section flags mark .rdata/.data executable, so select data sections by name instead.
	private static boolean isDataBlock(MemoryBlock b) {
		String n = b.getName();
		return b.isInitialized() && (n.equals(".rdata") || n.equals(".data") || n.equals("DOLBY") || n.startsWith("$$"));
	}

	private String stringAt(Address a) {
		try {
			Memory mem = currentProgram.getMemory();
			MemoryBlock b = mem.getBlock(a);
			if (b == null || !isDataBlock(b)) {
				return null;
			}
			StringBuilder sb = new StringBuilder();
			for (int i = 0; i < 200; i++) {
				byte c = mem.getByte(a.add(i));
				if (c == 0) {
					break;
				}
				if (c < 0x20 && c != '\n' && c != '\t' && c != '\r' || c >= 0x7f) {
					return null;
				}
				sb.append((char) c);
			}
			return sb.length() >= 2 ? sb.toString() : null;
		}
		catch (Exception e) {
			return null;
		}
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		try (PrintWriter out = new PrintWriter(args[0])) {
			out.println("function\tname\tstrings");
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				Set<String> strs = new LinkedHashSet<>();
				for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
					for (Reference r : ins.getReferencesFrom()) {
						String s = stringAt(r.getToAddress());
						if (s != null) {
							strs.add(s.replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r").replace("\u001f", ""));
						}
					}
				}
				if (!strs.isEmpty()) {
					out.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + String.join("\u001f", strs));
				}
			}
		}
		// {char *name, func *fn} tables (Lua luaL_reg style), at least 3 consecutive entries.
		Memory mem = currentProgram.getMemory();
		try (PrintWriter out = new PrintWriter(args[1])) {
			out.println("table\tindex\tstring\tfunction\tfunction_name");
			for (MemoryBlock b : mem.getBlocks()) {
				if (!isDataBlock(b)) {
					continue;
				}
				Address a = b.getStart();
				while (a.compareTo(b.getEnd().subtract(8)) < 0) {
					List<String[]> run = new ArrayList<>();
					Address p = a;
					while (p.compareTo(b.getEnd().subtract(8)) < 0) {
						long sp = mem.getInt(p) & 0xFFFFFFFFL;
						long fp = mem.getInt(p.add(4)) & 0xFFFFFFFFL;
						Address sa = toAddr(sp), fa = toAddr(fp);
						String s = mem.contains(sa) ? stringAt(sa) : null;
						MemoryBlock fb = mem.contains(fa) ? mem.getBlock(fa) : null;
						if (s == null || fb == null || !fb.getName().equals(".text") || !s.matches("[A-Za-z_][A-Za-z0-9_]*")) {
							break;
						}
						Function f = getFunctionAt(fa);
						run.add(new String[] { s, fa.toString(), f == null ? "(none)" : f.getName() });
						p = p.add(8);
					}
					if (run.size() >= 3) {
						for (int i = 0; i < run.size(); i++) {
							out.println(a + "\t" + i + "\t" + run.get(i)[0] + "\t" + run.get(i)[1] + "\t" + run.get(i)[2]);
						}
						a = p;
					}
					else {
						a = a.add(4);
					}
				}
			}
		}
	}
}
