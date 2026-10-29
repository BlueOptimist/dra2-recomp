// Marks the game's printf-style wrappers variadic. They hand "&stack0x0000000c" (the address after their last named
// parameter) to a vsnprintf; not knowing that, the decompiler shows every call with the named arguments only, so
// sprintf(buf, "menus/menu_%s_%s.res", name, language) loses name and language.
// Reads analysis/varargs.tsv (address, name, named parameters: from tools/find_varargs.py) and sets var-args on each
// function there with that many parameters. Re-export afterwards: the call sites then show the pushed arguments.
// Args: <varargs.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class SetVarargs extends GhidraScript {
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
				if (f.hasVarArgs() || f.getParameterCount() != Integer.parseInt(c[2])) {
					kept++;
					continue;
				}
				f.setVarArgs(true);
				f.setSignatureSource(SourceType.USER_DEFINED);
				changed++;
			}
		}
		println(String.format("variadic: %d set; already variadic or parameter count differs: %d; no function: %d",
			changed, kept, missing));
	}
}
