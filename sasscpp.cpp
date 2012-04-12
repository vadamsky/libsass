#include <iostream>
#include <string>
#include <map>
#include "document.hpp"
#include "eval_apply.hpp"

using namespace Sass;
using namespace std;

int main(int argc, char* argv[]) {
  if (argc < 2) {
    cout << "Hey, I at least need a file to read!" << endl;
    return 0;
  }
  
  char* path;
  Document::CSS_Style style = Document::nested;
  
  for (int i = 1; i < argc; ++i) {
    if (string(argv[i]) == "--style") {
      string style_name(argv[++i]);
      if (style_name == "nested") style = Document::nested;
      else if (style_name == "expanded") style = Document::expanded;
      else if (style_name == "echo") style = Document::echo;
    }
    else {
      path = argv[i];
    }
  }
    
  Document doc(path, 0);
  cerr << "INITIALIZED DOCUMENT OBJECT" << endl;

  doc.parse_scss();
  cerr << "PARSED DOCUMENT" << endl;

  eval(doc.root, doc.context.global_env, doc.context.function_env);
  cerr << "EVALUATED DOCUMENT" << endl;

  string output = doc.emit_css(style);
  
  // cerr << "Fresh nodes:\t" << Node::fresh << endl;
  // cerr << "Copied nodes:\t" << Node::copied << endl;
  cerr << "Allocations:\t" << Node::allocations << endl;
  cout << output;

  return 0;
}