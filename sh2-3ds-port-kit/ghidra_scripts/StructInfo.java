// Prints size and field offsets of named structures (any category). Args: <name> [max fields] ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import java.util.*;

public class StructInfo extends GhidraScript {
  public void run() throws Exception {
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    for (String arg : getScriptArgs()) {
      String[] p = arg.split(":");
      int max = p.length > 1 ? Integer.parseInt(p[1]) : 12;
      List<DataType> found = new ArrayList<>();
      dtm.findDataTypes(p[0], found);
      for (DataType dt : found) {
        if (!(dt instanceof Composite)) continue;
        Composite c = (Composite) dt;
        println(c.getCategoryPath() + "/" + c.getName() + " size " + c.getLength());
        int n = 0;
        for (DataTypeComponent comp : c.getDefinedComponents()) {
          if (n++ >= max) break;
          println(String.format("   +0x%03x %-28s %s", comp.getOffset(), comp.getFieldName(), comp.getDataType().getName()));
        }
        break;
      }
    }
  }
}
