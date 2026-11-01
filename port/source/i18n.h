#pragma once

// Text of the port's own screens (the touch screen's buttons and menus, the options' names, the setup
// pages, the radio ...) in the language the game runs in (ev_language(): 1 English, 2 French, 3 German,
// 4 Italian, 5 Spanish). The PC's messages already come in the language from the pack; what the port
// wrote itself is English in the source, and tr() maps that English text to the language in effect.
// Text without an entry, and English, come back unchanged, so tr() is safe on any string, including
// the PC's own. Translations are Windows-1252 bytes written as octal escapes (font.h).
//
// The result is valid for the life of the program (a table entry or the argument itself).
const char* tr(const char* english);
