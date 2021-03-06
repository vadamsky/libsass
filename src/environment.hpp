#ifndef SASS_ENVIRONMENT_H
#define SASS_ENVIRONMENT_H

#include <string>
#include <map>

#include "ast_fwd_decl.hpp"
#include "ast_def_macros.hpp"
#include "memory_manager.hpp"

namespace Sass {

  template <typename T>
  class Environment {
    // TODO: test with map
    std::map<std::string, T> local_frame_;
    ADD_PROPERTY(Environment*, parent)
    ADD_PROPERTY(bool, is_shadow)

  public:
    Memory_Manager mem;
    Environment(bool is_shadow = false);
    Environment(Environment* env, bool is_shadow = false);
    Environment(Environment& env, bool is_shadow = false);

    // link parent to create a stack
    void link(Environment& env);
    void link(Environment* env);

    // this is used to find the global frame
    // which is the second last on the stack
    bool is_lexical() const;

    // only match the real root scope
    // there is still a parent around
    // not sure what it is actually use for
    // I guess we store functions etc. there
    bool is_global() const;

    // scope operates on the current frame

    std::map<std::string, T>& local_frame();

    bool has_local(const std::string& key) const;

    T& get_local(const std::string& key);

    // set variable on the current frame
    void set_local(const std::string& key, T val);

    void del_local(const std::string& key);

    // global operates on the global frame
    // which is the second last on the stack
    Environment* global_env();
    // get the env where the variable already exists
    // if it does not yet exist, we return current env
    Environment* lexical_env(const std::string& key);

    bool has_global(const std::string& key);

    T& get_global(const std::string& key);

    // set a variable on the global frame
    void set_global(const std::string& key, T val);

    void del_global(const std::string& key);

    // see if we have a lexical variable
    // move down the stack but stop before we
    // reach the global frame (is not included)
    bool has_lexical(const std::string& key) const;

    // see if we have a lexical we could update
    // either update already existing lexical value
    // or we create a new one on the current frame
    void set_lexical(const std::string& key, T val);

    // look on the full stack for key
    // include all scopes available
    bool has(const std::string& key) const;

    // use array access for getter and setter functions
    T& operator[](const std::string& key);

    #ifdef DEBUG
    size_t print(std::string prefix = "");
    #endif

  };

}

#endif
