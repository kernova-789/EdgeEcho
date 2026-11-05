# Third-party notices

EdgeEcho includes SQLite 3.54.1 (`third_party/sqlite/sqlite3.c` and `sqlite3.h`),
which is in the public domain. The files were copied, without editing, from the
local EdgeSense Replay project's SQLite amalgamation. SQLite information and
license statement: https://www.sqlite.org/copyright.html .

The optional desktop executable dynamically links Qt 6 Core, Gui, Widgets and
Concurrent. Qt is not bundled in the source tree. Qt modules have their own
licenses; the open-source Qt distribution generally offers LGPLv3/GPLv3 and Qt
also offers commercial licensing. Anyone distributing a desktop binary must
review the licenses of the exact Qt build and deployment plugins, include the
required notices/license texts, and meet applicable relinking/source provisions.
See https://doc.qt.io/qt-6/licensing.html . No commercial Qt Charts module is used.

Local `.deps/` downloads used for verification are build dependencies, are ignored
by version control, and are not part of the EdgeEcho source distribution.

The MinGW cross-built command-line executable links the GCC/libstdc++ and
MinGW-w64 runtime libraries statically. Their licenses and the GCC Runtime
Library Exception apply to those runtime components; the executable does not
require separately installed MinGW runtime DLLs. See
https://www.gnu.org/licenses/gcc-exception-3.1.html and
https://www.mingw-w64.org/license/ . Native MSVC and Qt builds use their respective
compiler/runtime deployment terms.
