// Tags functions with their module from tools/module_map.py (module_map.tsv), e.g.
// "game/view/vc_main.c" or "lib/cri", plus "securom-encrypted" for code SecuROM keeps
// encrypted until runtime. Tags show in the Function Tags window and the listing, so the
// map can be filtered in the GUI. Old module tags are replaced.
// Args: <module_map.tsv>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.nio.file.*;
import java.util.*;

public class ApplyModuleMap extends GhidraScript {
  public void run() throws Exception {
    FunctionManager fm = currentProgram.getFunctionManager();
    int tagged = 0, enc = 0, missing = 0;
    for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
      String[] p = line.split("\t", -1);
      if (p[0].equals("entry")) continue;
      Function f = fm.getFunctionAt(toAddr(p[0]));
      if (f == null) { missing++; continue; }
      for (FunctionTag t : new ArrayList<>(f.getTags())) {
        String n = t.getName();
        if (n.startsWith("game/") || n.startsWith("lib/") || n.equals("import") || n.equals("securom-encrypted")) f.removeTag(n);
      }
      if (!p[2].isEmpty()) { f.addTag(p[2]); tagged++; }
      if (p.length > 4 && p[4].equals("yes")) { f.addTag("securom-encrypted"); enc++; }
    }
    println("module tags " + tagged + ", securom-encrypted " + enc + ", functions not found " + missing);
  }
}
