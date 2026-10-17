// For candidate names (label JSON), compares the PS2 signature's parameter count with the PC
// function's inferred parameter count. Prints one line per label: addr name pc_params ps2_params.
// Args: <labels.json> <ps2_functions.json>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class CheckSignatures extends GhidraScript {
  public void run() throws Exception {
    String labels = new String(Files.readAllBytes(Paths.get(getScriptArgs()[0])));
    String ps2 = new String(Files.readAllBytes(Paths.get(getScriptArgs()[1])));
    Map<String, Integer> ps2Params = new HashMap<>();
    Matcher sm = Pattern.compile("\"name\":\\s*\"(\\w+)\",\\s*\"signature\":\\s*\"([^\"]*)\"").matcher(ps2);
    while (sm.find()) {
      String args = sm.group(2).replaceAll("^.*?\\((.*)\\)\\s*$", "$1").trim();
      int n = args.isEmpty() || args.equals("void") ? 0 : args.split(",(?![^(]*\\))").length;
      ps2Params.putIfAbsent(sm.group(1), n);
    }
    Matcher m = Pattern.compile("\"addr\":\\s*\"(0x[0-9a-fA-F]+)\",\\s*\"name\":\\s*\"(\\w+)\"").matcher(labels);
    int agree = 0, total = 0;
    while (m.find()) {
      Function f = getFunctionAt(toAddr(m.group(1)));
      if (f == null) continue;
      Integer want = ps2Params.get(m.group(2));
      int have = f.getParameterCount();
      total++;
      if (want != null && want == have) agree++;
      println(m.group(1) + " " + m.group(2) + " pc=" + have + " ps2=" + want + (want != null && want == have ? "" : "  <--"));
    }
    println("parameter counts agree for " + agree + " of " + total);
  }
}
