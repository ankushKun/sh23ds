// Imports sh2_v10_labels.json (globals + functions) into the current Ghidra program.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.nio.file.*;
import java.util.regex.*;
public class ApplyLabels extends GhidraScript {
  public void run() throws Exception {
    String path = getScriptArgs().length>0 ? getScriptArgs()[0] : askFile("labels json","Open").getAbsolutePath();
    String j = new String(Files.readAllBytes(Paths.get(path)));
    Matcher m = Pattern.compile("\"addr\":\\s*\"(0x[0-9a-fA-F]+)\",\\s*\"name\":\\s*\"(\\w+)\"").matcher(j);
    int n=0;
    while (m.find()) {
      var a = toAddr(m.group(1)); String name = m.group(2);
      // a label that moved in the JSON: drop the old user-defined copy elsewhere
      for (Symbol old : currentProgram.getSymbolTable().getGlobalSymbols(name)) {
        if (!old.getAddress().equals(a) && old.getSource() == SourceType.USER_DEFINED && old.getSymbolType() == SymbolType.LABEL) old.delete();
      }
      Function f = getFunctionAt(a);
      boolean isFunc = name.matches("^[A-Z][a-z]+_.*");
      // code Ghidra never reached (e.g. Win_CreateWindowAndDInput): disassemble before making the function
      if (isFunc && getInstructionAt(a)==null) disassemble(a);
      if (f!=null && isFunc && f.getBody().getNumAddresses()<=1) ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
      if (f==null && isFunc) f = createFunction(a, null);
      if (f!=null) f.setName(name, SourceType.USER_DEFINED);
      else { currentProgram.getSymbolTable().createLabel(a, name, SourceType.USER_DEFINED).setPrimary(); }
      n++;
    }
    println("applied "+n+" labels");
  }
}
