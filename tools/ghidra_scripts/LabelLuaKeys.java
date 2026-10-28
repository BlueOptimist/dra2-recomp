// Labels Lua config key strings (from tools/lua_bindings.py) and annotates the functions that read them.
// Args: <lua_keys.tsv> <report.tsv>
// - each key string gets the label luakey_<key>
// - each referencing function gets a plate comment listing the keys it reads
// - an unnamed (FUN_) function reading >= 3 keys of a single table is renamed lua_<table>_<address>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class LabelLuaKeys extends GhidraScript {
	private static final int MIN_KEYS_TO_RENAME = 3;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Map<Function, Set<String>> keysByFunction = new TreeMap<>((a, b) -> a.getEntryPoint().compareTo(b.getEntryPoint()));
		Map<String, Set<String>> tablesByKey = new TreeMap<>();
		int labeled = 0, referenced = 0;

		try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] cols = line.split("\t");
				String key = cols[0];
				Set<String> tables = new TreeSet<>(List.of(cols[1].split(",")));
				tablesByKey.put(key, tables);
				boolean hasRefs = false;
				for (String a : cols[3].split(",")) {
					Address addr = toAddr(Long.decode(a));
					createLabel(addr, "luakey_" + key, true, SourceType.ANALYSIS);
					labeled++;
					for (Reference ref : getReferencesTo(addr)) {
						Function f = getFunctionContaining(ref.getFromAddress());
						if (f != null) {
							keysByFunction.computeIfAbsent(f, k -> new TreeSet<>()).add(key);
							hasRefs = true;
						}
					}
				}
				if (hasRefs) {
					referenced++;
				}
			}
		}

		int renamed = 0;
		try (PrintWriter out = new PrintWriter(args[1])) {
			out.println("function\tname\tkey_count\ttables\tkeys");
			for (Map.Entry<Function, Set<String>> e : keysByFunction.entrySet()) {
				Function f = e.getKey();
				Set<String> keys = e.getValue();
				Set<String> tables = commonTables(keys, tablesByKey);
				if (f.getName().startsWith("FUN_") && keys.size() >= MIN_KEYS_TO_RENAME && tables.size() == 1) {
					String table = tables.iterator().next().replaceAll("[^A-Za-z0-9_]", "");
					f.setName("lua_" + table + "_" + f.getEntryPoint(), SourceType.ANALYSIS);
					renamed++;
				}
				f.setComment("Reads Lua keys: " + String.join(", ", keys));
				out.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + keys.size() + "\t" +
					String.join(",", tables) + "\t" + String.join(",", keys));
			}
		}
		println(String.format("labeled %d strings, %d keys referenced from code, %d functions annotated, %d renamed",
			labeled, referenced, keysByFunction.size(), renamed));
	}

	// Tables shared by every key; falls back to the most common table when keys disagree.
	private Set<String> commonTables(Set<String> keys, Map<String, Set<String>> tablesByKey) {
		Set<String> common = null;
		Map<String, Integer> votes = new TreeMap<>();
		for (String k : keys) {
			Set<String> t = new TreeSet<>(tablesByKey.get(k));
			t.remove("<global>");
			for (String x : t) {
				votes.merge(x, 1, Integer::sum);
			}
			if (t.isEmpty()) {
				continue;
			}
			if (common == null) {
				common = t;
			}
			else {
				common.retainAll(t);
			}
		}
		if (common != null && common.size() == 1) {
			return common;
		}
		List<String> all = new ArrayList<>(votes.keySet());
		return new TreeSet<>(all);
	}
}
