#include "precompiled.hpp"
#include "classfile/vmSymbols.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/vmClasses.hpp"
#include "memory/oopFactory.hpp"
#include "runtime/arguments.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/quickStart.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/defaultStream.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.hpp"
#include "runtime/handles.inline.hpp"

bool QuickStart::_is_starting = true;
bool QuickStart::_is_enabled = false;
bool QuickStart::_verbose = false;
bool QuickStart::_print_stat_enabled = false;
bool QuickStart::_need_destroy = false;
bool QuickStart::_profile_only = false;
bool QuickStart::_dump_only = false;
bool QuickStart::_replay_only = false;

QuickStart::QuickStartRole QuickStart::_role = QuickStart::Normal;

const char* QuickStart::_cache_path = NULL;
const char* QuickStart::_image_id = NULL;
const char* QuickStart::_vm_version = NULL;
const char* QuickStart::_lock_path = NULL;
const char* QuickStart::_temp_metadata_file_path = NULL;
const char* QuickStart::_metadata_file_path = NULL;

const char* QuickStart::_origin_class_list = "cds_origin_class.lst";
const char* QuickStart::_final_class_list = "cds_final_class.lst";
const char* QuickStart::_jsa = "cds.jsa";
const char* QuickStart::_eagerappcds_agent = NULL;
const char* QuickStart::_eagerappcds_agentlib = NULL;
const char* QuickStart::_module_filename      = "modules.lst";

int QuickStart::_jvm_option_count = 0;
const char** QuickStart::_jvm_options = NULL;
const char*  QuickStart::_cp_in_metadata_file = NULL;

const char* QuickStart::_opt_name[] = {
#define OPT_TAG(name) #name,
  OPT_TAG_LIST
#undef OPT_TAG
};

enum identifier {
  Features,
  VMVersion,
  ContainerImageID,
  JVMOptionCount,
  ClassPathLength
};

const char* QuickStart::_identifier_name[] = {
  "Features: ",
  "VM_Version: ",
  "Container_Image_ID: ",
  "JVM_Option_Count: ",
  "Class_Path_Length: "
};

bool QuickStart::_opt_enabled[] = {
  true,  // appcds
  true,  // eagerappcds
};

bool QuickStart::_opt_passed[] = {
  false,
  false,
};

FILE* QuickStart::_metadata_file = NULL;
fileStream* QuickStart::_temp_metadata_file = NULL;

int QuickStart::_lock_file_fd = 0;

#define DEFAULT_SHARED_DIRECTORY      "alibaba.quickstart.sharedcache"
#define METADATA_FILE                 "metadata"
// metadata file stores some information for invalid check, and generate it after the startup is complete.
// Before the content of the file is completely generated, use the file name metadata.tmp temporarily.
// After the startup is complete, rename metadata.tmp to metadata.
#define TEMP_METADATA_FILE            "metadata.tmp"
#define LOCK_FILE                     "LOCK"
#define CDS_DIFF_CLASSES              "cds_diff_classes.lst"

bool QuickStart::parse_command_line_arguments(const char* options) {
  static const char *first_level_options[] = {
    "help",
    "destroy",
    "printStat",
    "satDump",
    "profile",
    "dump",
    "replay"
  };
  _is_enabled = true;
  if (options == NULL) {
    return true;
  }
  char* copy = os::strdup_check_oom(options, mtArguments);

  // Split string on commas
  bool success = true;
  for (char *comma_pos = copy, *cur = copy; success && comma_pos != NULL; cur = comma_pos + 1) {
    comma_pos = strchr(cur, ',');
    if (comma_pos != NULL) {
      *comma_pos = '\0';
    }
    const char* tail = NULL;
    if (*cur == '+') {
      success = set_optimization(cur + 1, true);
    } else if (*cur == '-') {
      success = set_optimization(cur + 1, false);
    } else if (match_option(cur, "help", &tail)) {
      fileStream stream(defaultStream::output_stream());
      print_command_line_help(&stream);
      vm_exit(0);
    } else if (match_option(cur, "verbose", &tail)) {
      if (tail[0] != '\0') {
        success = false;
        tty->print_cr("[QuickStart] Invalid -Xquickstart option '%s'", cur);
      }
      _verbose = true;
    } else if (match_option(cur, "printStat", &tail)) {
      if (tail[0] != '\0') {
        success = false;
        tty->print_cr("[QuickStart] Invalid -Xquickstart option '%s'", cur);
      }
      _print_stat_enabled = true;
    } else if (match_option(cur, "destroy", &tail)) {
      if (tail[0] != '\0') {
        success = false;
        tty->print_cr("[QuickStart] Invalid -Xquickstart option '%s'", cur);
      }
      _need_destroy = true;
    } else if (match_option(cur, "path=", &tail)) {
      _cache_path = os::strdup_check_oom(tail, mtArguments);
    } else if (match_option(cur, "containerImageEnv=", &tail)) {
      char *buffer = ::getenv(tail);
      if (buffer != NULL) {
        _image_id = os::strdup_check_oom(buffer, mtArguments);
      }
    } else if (match_option(cur, "profile", &tail)) {
      if (tail[0] != '\0') {
        success = false;
        log_error(quickstart)("Invalid -Xquickstart option '%s'", cur);
      }
      _profile_only = true;
    } else if (match_option(cur, "dump", &tail)) {
      if (tail[0] != '\0') {
        success = false;
        log_error(quickstart)("Invalid -Xquickstart option '%s'", cur);
      }
      _dump_only = true;
    } else if (match_option(cur, "replay", &tail)) {
      if (tail[0] != '\0') {
        success = false;
        log_error(quickstart)("Invalid -Xquickstart option '%s'", cur);
      }
      _replay_only = true;
    } else {
      success = false;
      tty->print_cr("[QuickStart] Invalid -Xquickstart option '%s'", cur);
    }
  }

  
  
  if (EagerAppCDSStaticClassDiffCheck && !_opt_enabled[_eagerappcds]) {
    success = false;
    log_error(quickstart)("Delta mode only enable for eagerappcds");
  }
  os::free(copy);
  return success;
}

bool QuickStart::set_optimization(const char* option, bool enabled) {
  for (int i = 0; i < QuickStart::Count; i++) {
    if (strcasecmp(option, _opt_name[i]) == 0) {
      _opt_enabled[i] = enabled;
      return true;
    }
  }

  tty->print_cr("[QuickStart] Invalid -Xquickstart optimization option '%s'", option);
  return false;
}

bool QuickStart::match_option(const char* option, const char* name, const char** tail) {
  size_t len = strlen(name);
  if (strncmp(option, name, len) == 0) {
    *tail = option + len;
    return true;
  } else {
    return false;
  }
}

void QuickStart::print_command_line_help(outputStream* out) {
  out->print_cr("-Xquickstart Usage: -Xquickstart[:option,...]");
  out->cr();

  out->print_cr("Available option:");
  out->cr();

  out->print_cr("  help                 Print general quick start help");
  out->print_cr("  verbose              Enable verbose output");
  out->print_cr("  printStat            List all the elements in the cache");
  out->print_cr("  path=<path>          Specify the location of the cache files");
  out->print_cr("  destroy              Destroy the cache files (use specified path or default)");
  out->print_cr("  +/-<opt>             Enable/disable the specific optimization");
  out->print_cr("  containerImageEnv=<env>   Specify the environment variable to get the unique identifier of the container");
  out->print_cr("  profile                   Record profile information and save in the cache");
  out->print_cr("  dump                      Dump using profile information in the cache");
  out->cr();

  out->print_cr("Available optimization:");
  out->cr();
}

// initialize JDK part for QuickStart
void QuickStart::initialize(TRAPS) {
  Klass* klass = vmClasses::com_alibaba_util_QuickStart_klass();
  JavaValue result(T_VOID);
  JavaCallArguments args(5);
  args.push_int(_role);
  args.push_oop(java_lang_String::create_from_str(QuickStart::cache_path(), THREAD));
  args.push_int(QuickStart::_verbose);
  args.push_oop(is_dumper() ? jvm_option_handle(THREAD) : objArrayHandle());
  args.push_oop(is_dumper() ? java_lang_String::create_from_str(_cp_in_metadata_file, THREAD) : Handle());

  JavaCalls::call_static(&result, klass, vmSymbols::initialize_name(),
                         vmSymbols::int_string_bool_stringarray_string_void_signature(), &args, CHECK);

  if (is_tracer()) {
    add_dump_hook(CHECK);
  } else if (is_profiler()) {

  } else if (is_dumper()) {
    add_dump_hook(CHECK);
    Klass *klass = vmClasses::com_alibaba_util_QuickStart_klass();
    JavaValue result(T_VOID);
    JavaCallArguments args(0);

    JavaCalls::call_static(&result, klass, vmSymbols::notifyDump_name(),
                           vmSymbols::void_method_signature(), &args, THREAD);
    vm_exit(0);
  }
}

void QuickStart::add_dump_hook(TRAPS) {
  if (_opt_enabled[_eagerappcds] || _opt_enabled[_appcds]) {
    add_CDSDumpHook(CHECK);
  }
}

Handle QuickStart::jvm_option_handle(TRAPS) {
  InstanceKlass *ik = vmClasses::String_klass();
  objArrayOop r = oopFactory::new_objArray(ik, _jvm_option_count, CHECK_NH);
  objArrayHandle result_h(THREAD, r);
  if (_jvm_option_count != 0 && _jvm_options != NULL) {
    for (int i = 0; i < _jvm_option_count; i++) {
      Handle h = java_lang_String::create_from_platform_dependent_str(_jvm_options[i], CHECK_NH);
      result_h->obj_at_put(i, h());
    }
  }
  return result_h;
}

void QuickStart::add_CDSDumpHook(TRAPS) {
  Klass *klass = vmClasses::com_alibaba_util_CDSDumpHook_klass();
  JavaValue result(T_VOID);
  JavaCallArguments args(5);
  args.push_oop(java_lang_String::create_from_str(QuickStart::_origin_class_list, THREAD));
  args.push_oop(java_lang_String::create_from_str(QuickStart::_final_class_list, THREAD));
  args.push_oop(java_lang_String::create_from_str(QuickStart::_jsa, THREAD));
  args.push_oop(java_lang_String::create_from_str(QuickStart::_eagerappcds_agent, THREAD));
  args.push_int(_opt_enabled[_eagerappcds]);
  JavaCalls::call_static(&result, klass, vmSymbols::initialize_name(),
                         vmSymbols::string_string_string_string_bool_void_signature(), &args, CHECK);
}

void QuickStart::post_process_arguments(JavaVMInitArgs* options_args) {
  // Prepare environment
  calculate_cache_path();
  // destroy the cache directory
  destroy_cache_folder();
  if (_dump_only) {
    if (!prepare_dump(options_args)) {
      vm_exit(1);
    }
  } else {
    // Determine the role
    if (!determine_role(options_args)) {
      _role = Normal;
      _is_enabled = false;
      setenv_for_roles();
      return;
    }
  }
  settle_opt_pass_table();
  // Process argument for each optimization
  process_argument_for_optimization();
}

bool QuickStart::check_integrity(JavaVMInitArgs* options_args, const char* meta_file) {
  if (_print_stat_enabled) {
    print_stat(true);
  }

  _metadata_file = os::fopen(meta_file, "r");
  if (!_metadata_file) {
    // if one process removes metadata here, will NULL.
    log("metadata file may be destroyed by another process.");
    return false;
  }
  bool result = load_and_validate(options_args);

  ::fclose(_metadata_file);
  if (result && _opt_enabled[_eagerappcds] && EagerAppCDSStaticClassDiffCheck) {
    char buf[PATH_MAX];
    struct stat st;
    jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), CDS_DIFF_CLASSES);
    int ret = os::stat(buf,&st);
    if (ret != 0) {
      result = false;
      log_error(quickstart)("Turn on EagerAppCDSStaticClassDiffCheck,but not found class diff file: %s",buf);
    } else {
      Arguments::set_invalid_class_path(os::strdup_check_oom(buf, mtArguments));
    }
  }
  return result;
}

void QuickStart::check_features(const char* &str) {
  // read features
  bool tracer_features[QuickStart::Count] = {};
  for (int begin = 0, end = 0; ; ) {
    bool exit = false;
    if (str[end] == ',' || (exit = (str[end] == '\n'))) {
      // handle feature
      for (int i = 0; i < QuickStart::Count; i++) {
        int len = MIN2(::strlen(&str[begin]), ::strlen(_opt_name[i]));
        if (::strncmp(&str[begin], _opt_name[i], len) == 0) {
          // find a feature which is enabled at tracer phase.
          tracer_features[i] = true;
        }
      }
      if (exit) {
        const_cast<char*>(str)[end] = '\n';
        break;
      }
      // the next feature: align pointers
      begin = ++end;
    } else {
      end++;
    }
  }
  for (int i = 0; i < QuickStart::Count; i++) {
    if (!tracer_features[i] && _opt_enabled[i]) {
      _opt_enabled[i] = false;
    }
    //when run with dump, features not pass by arguments,but by metadata file
    if (_dump_only) {
      if (tracer_features[i]) {
        _opt_enabled[i] = true;
      }
    }
  }
}

bool QuickStart::load_and_validate(JavaVMInitArgs* options_args) {
  char line[PATH_MAX];
  const char* tail          = NULL;
  bool feature_checked      = false;
  bool version_checked      = false;
  bool container_checked    = false;
  bool option_checked       = false;
  bool cp_len_checked       = false;

  _vm_version = VM_Version::internal_vm_info_string();

  while (fgets(line, sizeof(line), _metadata_file) != NULL) {

    if (!feature_checked && match_option(line, _identifier_name[Features], &tail)) {
      check_features(tail);
      feature_checked = true;
    } else if (!version_checked && match_option(line, _identifier_name[VMVersion], &tail)) {
      // read jvm info
      if (options_args != NULL && strncmp(tail, _vm_version, strlen(_vm_version)) != 0) {
        log("VM Version isn't the same.");
        return false;
      }
      version_checked = true;
    } else if (!container_checked && match_option(line, _identifier_name[ContainerImageID], &tail)) {
      container_checked = true;
      // read image info
      if (options_args == NULL) {
        continue;
      }
      // ignore \n
      int size = strlen(tail) - 1;
      const char *image_ident = QuickStart::image_id();
      int ident_size = image_ident != NULL ? strlen(image_ident) : 0;
      if (size != ident_size) {
        QuickStart::log("Container image isn't the same.");
        return false;
      }

      if (strncmp(tail, QuickStart::image_id(), size) != 0) {
        log("Container image isn't the same.");
        return false;
      }
    } else if (!option_checked && match_option(line, _identifier_name[JVMOptionCount], &tail)) {
      // read previous jvm options count
      if (sscanf(tail, "%d", &_jvm_option_count) != 1) {
        log_error(quickstart)("Unable to read the option number.");
        return false;
      }
      option_checked = true;
      // We delegate argument checking logic to CDS and AOT themselves.
      // Just sanity check reading the file and ignore the results.
      // Note: at this time of argument parsing, we cannot use Thread local and ResourceMark
      for (int index = 0; index < _jvm_option_count; index++) {
        if (fgets(line, sizeof(line), _metadata_file) == NULL) {
          log_error(quickstart)("Unable to read JVM option.");
          return false;
        } else if (_dump_only) {
          //when run with dump stage.JVM option and features only can get from metadata
          //cannot get from arguments.
          if (_jvm_options == NULL) {
            _jvm_options = NEW_C_HEAP_ARRAY(const char*, _jvm_option_count, mtArguments);
          }
          trim_tail_newline(line);
          _jvm_options[index] = os::strdup_check_oom(line);
        }
      }
    } else if (!cp_len_checked && match_option(line, _identifier_name[ClassPathLength], &tail)) {
      int cp_len = 0;
      if (sscanf(tail, "%d", &cp_len) != 1) {
        tty->print_cr("Unable read class path length.");
        return false;
      }
      if (cp_len <= 0 || (size_t)cp_len > 64 * M) {
        tty->print_cr("Invalid \"%s\"'s value: %d.It should > 0 and <= 64M.", _identifier_name[ClassPathLength], cp_len);
        return false;
      }

      cp_len_checked = true;
      //+2,one for new line, one for \0
      cp_len += 2;
      char *cp_buff = NEW_C_HEAP_ARRAY(char, cp_len, mtArguments);
      if (fgets(cp_buff, cp_len, _metadata_file) == NULL) {
        tty->print_cr("Unable to read classpath option.");
        FREE_C_HEAP_ARRAY(char, cp_buff);
        return false;
      }
      if (cp_buff[cp_len - 2] != '\n') {
        tty->print_cr("ClassPath real size not match with expect size.");
        FREE_C_HEAP_ARRAY(char, cp_buff);
        return false;
      }

      trim_tail_newline(cp_buff);
      _cp_in_metadata_file = os::strdup_check_oom(cp_buff);
      FREE_C_HEAP_ARRAY(char, cp_buff);
    }
  }
  return true;
}

void QuickStart::trim_tail_newline(char *str) {
  int len = (int)strlen(str);
  int i;
  // Replace \t\r\n with ' '
  for (i = 0; i < len; i++) {
    if (str[i] == '\t' || str[i] == '\r' || str[i] == '\n') {
      str[i] = ' ';
    }
  }

  // Remove trailing newline/space
  while (len > 0) {
    if (str[len-1] == ' ') {
      str[len-1] = '\0';
      len --;
    } else {
      break;
    }
  }
}

void QuickStart::calculate_cache_path() {
  if (_cache_path != NULL) {
    log("cache path is set from -Xquickstart:path=%s", _cache_path);
    return;
  }

  const char *buffer = ::getenv("QUICKSTART_CACHE_PATH");
  if (buffer != NULL && (_cache_path = os::strdup_check_oom(buffer)) != NULL) {
    log("cache path is set from env with %s", _cache_path);
    return;
  }
  const char* home = ::getenv("HOME");
  char buf_pwd[O_BUFLEN];
  if (home == NULL) {
    home = os::get_current_directory(buf_pwd, sizeof(buf_pwd));
    if (home == NULL) {
      log_error(quickstart)("neither HOME env nor current_dir is available");
      vm_exit(1);
    }
  }
  char buf[PATH_MAX];
  jio_snprintf(buf, PATH_MAX, "%s%s%s", home, os::file_separator(), DEFAULT_SHARED_DIRECTORY);
  _cache_path = os::strdup_check_oom(buf);
  log("cache path is set as default with %s", _cache_path);
}

void QuickStart::destroy_cache_folder() {
  if (_need_destroy && _cache_path != NULL) {
    if (remove_dir(_cache_path) < 0) {
      log("failed to destory the cache folder: %s", _cache_path);
    } else {
      log("destory the cache folder: %s", _cache_path);
    }
    vm_exit(0);
  }

  //when profile enable,always clear cache path first which prepare for profiling again.
  if (_profile_only && _cache_path != NULL) {
    if (!os::dir_is_empty(_cache_path)) {
      if (remove_dir(_cache_path) < 0) {
        log_error(quickstart)("failed to destroy the cache folder: %s", _cache_path);
      } else {
        log_info(quickstart)("destroy the cache folder: %s", _cache_path);
      }
    }
  }
}

void QuickStart::print_stat(bool isReplayer) {
  if (!_print_stat_enabled) {
    return;
  }
  if (isReplayer) {
    _metadata_file = os::fopen(_metadata_file_path, "r");
    if (_metadata_file) {
      bool result = load_and_validate(NULL);
      ::fclose(_metadata_file);
      if (result) {
        // print cache information for replayer
        jio_fprintf(defaultStream::output_stream(), "[QuickStart] Current statistics for cache %s\n", _cache_path);
        jio_fprintf(defaultStream::output_stream(), "\n");
        jio_fprintf(defaultStream::output_stream(), "Cache created with:\n");
        vm_exit(0);
      }
    }
  }

  jio_fprintf(defaultStream::output_stream(), "[QuickStart] There is no cache in %s\n", _cache_path);
  vm_exit(0);
}

void QuickStart::setenv_for_roles() {
  const char *role = NULL;
  if (_role == Tracer) {
    role = "TRACER";
  } else if (_role == Replayer) {
    role = "REPLAYER";
  } else if (_role == Normal) {
    role = "NORMAL";
  } else if (_role == Profiler) {
    role = "PROFILER";
  } else if (_role == Dumper) {
    role = "DUMPER";
  } else {
    ShouldNotReachHere();
  }
  setenv("ALIBABA_QUICKSTART_ROLE", role, 1);
}

void QuickStart::process_argument_for_optimization() {
  if (_role == Tracer || _role == Replayer || _role == Profiler || _role == Dumper) {
    if (_opt_enabled[_eagerappcds]) {
      QuickStart::enable_appcds();
      QuickStart::enable_eagerappcds();
    } else if (_opt_enabled[_appcds]) {
      // only if we set -eagerappcds can we go here
      QuickStart::enable_appcds();
    }
  }
}

void QuickStart::enable_eagerappcds() {
  FLAG_SET_CMDLINE(EagerAppCDS, true);

  if (!_eagerappcds_agent) {
    char buf[JVM_MAXPATHLEN];
    sprintf(buf, "serverless%sserverless-adapter.jar", os::file_separator());
    // The real path: <JDK_HOME>/lib/serverless/CDSXMLAutoRegisterAgent.jar
    _eagerappcds_agent = strdup(buf);
  }
  if (!_eagerappcds_agentlib) {
    char buf[JVM_MAXPATHLEN];
    sprintf(buf, "serverless%slibloadclassagent.so", os::file_separator());
    // The real path: <JDK_HOME>/lib/serverless/libloadclassagent.so
    _eagerappcds_agentlib = strdup(buf);
  }

  char buf[JVM_MAXPATHLEN];
  sprintf(buf, "%s%slib%s%s", Arguments::get_java_home(), os::file_separator(), os::file_separator(), _eagerappcds_agent);
  Arguments::append_sysclasspath(buf);
  sprintf(buf, "%s%slib%s%s", Arguments::get_java_home(), os::file_separator(), os::file_separator(), _eagerappcds_agentlib);
  Arguments::add_init_agent(buf, NULL, true);
}

void QuickStart::enable_appcds() {
  char buf[JVM_MAXPATHLEN];
  if (QuickStart::is_tracer() || QuickStart::is_profiler()) {
    FLAG_SET_CMDLINE(UseSharedSpaces, false);
    FLAG_SET_CMDLINE(RequireSharedSpaces, false);
    sprintf(buf, "%s%s%s", QuickStart::cache_path(), os::file_separator(), _origin_class_list);
    DumpLoadedClassList = strdup(buf);
  } else if (QuickStart::is_replayer()) {
    FLAG_SET_CMDLINE(UseSharedSpaces, true);
    FLAG_SET_CMDLINE(RequireSharedSpaces, true);
    sprintf(buf, "%s%s%s", QuickStart::cache_path(), os::file_separator(), _jsa);
    SharedArchiveFile = strdup(buf);
  } else if (QuickStart::is_dumper()) {
    sprintf(buf, "%s%s%s", QuickStart::cache_path(), os::file_separator(), _jsa);
  }
  FLAG_SET_CMDLINE(AppCDSLegacyVerisonSupport, true);
  FLAG_SET_CMDLINE(DumpAppCDSWithKlassId, true);
}

bool QuickStart::determine_role(JavaVMInitArgs* options_args) {
  struct stat st;
  char buf[PATH_MAX];
  int ret = os::stat(_cache_path, &st);
  if (ret != 0) {
    if (_print_stat_enabled) {
      print_stat(false);
    }
    if (_replay_only) {
      log_info(quickstart)("QuickStart replay role is specified without shared directory found. Running as a normal process with quickstart disabled.");
      return false;
    }
    ret = ::mkdir(_cache_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    if (ret != 0) {
      log("Could not mkdir [%s] because [%s]", _cache_path, os::strerror(errno));
      return false;
    }
  } else if (!S_ISDIR(st.st_mode)) {
    log("Cache path [%s] is not a directory, "
        "please use -Xquickstart:path=<path> or environment variable "
        "QUICKSTART_CACHE_PATH to specify.\n",
        _cache_path);
    return false;
  }

  // check whether the metadata file exists.
  jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), METADATA_FILE);
  _metadata_file_path = os::strdup_check_oom(buf, mtArguments);
  ret = os::stat(_metadata_file_path, &st);
  if (ret < 0 && errno == ENOENT) {
    if (_print_stat_enabled) {
      print_stat(false);
    }
    // Create a LOCK file
    jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), LOCK_FILE);
    _lock_path = os::strdup_check_oom(buf, mtArguments);
    // if the lock exists, it returns -1.
    _lock_file_fd = os::create_binary_file(_lock_path, false);
    if (_lock_file_fd == -1) {
      log("Fail to create LOCK file");
      return false;
    }
    jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), TEMP_METADATA_FILE);
    _temp_metadata_file_path = os::strdup_check_oom(buf, mtArguments);
    ret = os::stat(buf, &st);
    if (ret == 0) {
      // error: A file exists, determine failed. Maybe this is a user's file.
      jio_fprintf(defaultStream::error_stream(), "[QuickStart] The %s file exists\n", TEMP_METADATA_FILE);
      return false;
    }
    _temp_metadata_file = new(ResourceObj::C_HEAP, mtInternal) fileStream(_temp_metadata_file_path, "w");
    if (!_temp_metadata_file) {
      jio_fprintf(defaultStream::error_stream(), "[QuickStart] Failed to create %s file\n", TEMP_METADATA_FILE);
      return false;
    }
    if (!dump_cached_info(options_args)) {
      jio_fprintf(defaultStream::error_stream(), "[QuickStart] Failed to dump cached information\n");
      return false;
    }
    if (_profile_only) {
      _role = Profiler;
      log_info(quickstart)("Running as profiler");
    } else {
      _role = Tracer;
      log_info(quickstart)("Running as tracer");
    }
    return true;
  } else if (ret == 0 && check_integrity(options_args, _metadata_file_path)) {
    _role = Replayer;
    log("Running as replayer");
    return true;
  }
  return false;
}

bool QuickStart::prepare_dump(JavaVMInitArgs *options_args) {
  struct stat st;
  char buf[PATH_MAX];
  // check whether the metadata file exists.
  // when run quickstart with profile,then dump.In the profile stage,the metadata.tmp not rename to metadata.
  jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), TEMP_METADATA_FILE);
  _temp_metadata_file_path = os::strdup_check_oom(buf, mtArguments);
  int ret = os::stat(_temp_metadata_file_path, &st);
  if (ret < 0 && errno == ENOENT) {
    log_error(quickstart)("The %s file not exists\n", _temp_metadata_file_path);
    return false;
  } else if (ret == 0 && check_integrity(options_args, _temp_metadata_file_path)) {
    // Create a LOCK file
    jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), LOCK_FILE);
    _lock_path = os::strdup_check_oom(buf, mtArguments);
    // if the lock exists, it returns -1.
    _lock_file_fd = os::create_binary_file(_lock_path, false);
    if (_lock_file_fd == -1) {
      log_error(quickstart)("Fail to create LOCK file");
      return false;
    }
    jio_snprintf(buf, PATH_MAX, "%s%s%s", _cache_path, os::file_separator(), METADATA_FILE);
    _metadata_file_path = os::strdup_check_oom(buf, mtArguments);
    _role = Dumper;
    log_info(quickstart)("Running as dumper");
    return true;
  }
  log_error(quickstart)("Cannot dump,maybe the %s is invalid!ret: %d", TEMP_METADATA_FILE, ret);
  return false;
}

void QuickStart::settle_opt_pass_table() {
  // If a feature is disabled by quickstart, we have no need to check it
  // So it is directly passed - so set it to true (already passed).
  // If a feature is settled to be enabled by quickstart, then we need to
  // check if it is successfully passed - so set it to false (not passed yet).
  _opt_passed[_eagerappcds] = !_opt_enabled[_eagerappcds];
  _opt_passed[_appcds]      = !_opt_enabled[_appcds];
}

void QuickStart::set_opt_passed(opt feature) {
  // set a feature passed
  _opt_passed[feature] = true;
  log_info(quickstart)("feature %s is enabled and passed", _opt_name[feature]);

  // If all features are passed, we set the environment for roles of this process
  bool opt_all_passed = true;
  for (int i = 0; i < Count; i++) {
    opt_all_passed &= _opt_passed[i];
  }
  if (opt_all_passed) {
    log_info(quickstart)("all enabled features are passed");
    setenv_for_roles();
  }
}

void QuickStart::generate_metadata_file(bool rename_metafile) {
  // mv metadata.tmp to metadata
  delete _temp_metadata_file;
  int ret;
  if (rename_metafile) {
    ret = ::rename(_temp_metadata_file_path, _metadata_file_path);
    if (ret != 0) {
      log_error(quickstart)("Could not mv [%s] to [%s] because [%s]\n",
                            TEMP_METADATA_FILE, METADATA_FILE, os::strerror(errno));
    }
  }

  // remove lock file
  ret = ::close(_lock_file_fd);
  if (ret != 0) {
    jio_fprintf(defaultStream::error_stream(),
                "[QuickStart] Could not close [%s] because [%s]\n",
                LOCK_FILE,
                os::strerror(errno));
  }

  ret = ::remove(_lock_path);
  if (ret != 0) {
    jio_fprintf(defaultStream::error_stream(),
                "[QuickStart] Could not delete [%s] because [%s]\n",
                LOCK_FILE,
                os::strerror(errno));
  }
}

void QuickStart::log(const char* msg, ...) {
  if (_verbose) {
    va_list ap;
    va_start(ap, msg);
    tty->print("[QuickStart(%d)] ", os::current_process_id());
    tty->vprint(msg, ap);
    tty->print_cr("");
    va_end(ap);
  }
}

int QuickStart::remove_dir(const char* dir) {
  char cur_dir[] = ".";
  char up_dir[]  = "..";
  char dir_name[PATH_MAX];
  DIR *dirp = NULL;
  struct dirent *dp;
  struct stat dir_stat;

  int ret = os::stat(dir, &dir_stat);
  if (ret < 0) {
    jio_fprintf(defaultStream::error_stream(),
                "[QuickStart] Fail to get the stat for directory %s\n",
                dir);
    return ret;
  }

  if (S_ISREG(dir_stat.st_mode)) {
    ret = ::remove(dir);
  } else if (S_ISDIR(dir_stat.st_mode)) {
    dirp = os::opendir(dir);
    while ((dp = os::readdir(dirp)) != NULL) {
      if ((strcmp(cur_dir, dp->d_name) == 0) || (strcmp(up_dir, dp->d_name) == 0)) {
        continue;
      }
      jio_snprintf(dir_name, PATH_MAX, "%s%s%s", dir, os::file_separator(), dp->d_name);
      ret = remove_dir(dir_name);
      if (ret != 0) {
        break;
      }
    }

    os::closedir(dirp);
    if (ret != 0) {
      return -1;
    }
    ret = ::rmdir(dir);
  } else {
    jio_fprintf(defaultStream::error_stream(), "[QuickStart] unknow file type\n");
  }
  return ret;
}

void QuickStart::notify_dump() {
  if (_role == Tracer || _role == Profiler || _role == Dumper) {
    generate_metadata_file(_role != Profiler);
  }
  log("notifying dump done.");
}

bool QuickStart::dump_cached_info(JavaVMInitArgs* options_args) {
  if (_temp_metadata_file == NULL) {
    return false;
  }
  _vm_version = VM_Version::internal_vm_info_string();

  // calculate argument, ignore the last two option:
  // -Dsun.java.launcher=SUN_STANDARD
  // -Dsun.java.launcher.pid=<pid>
  _jvm_option_count = options_args->nOptions > 2 ? options_args->nOptions - 2 : 0;

  _temp_metadata_file->print("%s", _identifier_name[Features]);
  // write features
  for (int i = 0, cnt = 0; i < QuickStart::Count; i++) {
    if (_opt_enabled[i]) {
      if (cnt++ == 0) {
        _temp_metadata_file->print("%s", _opt_name[i]);
      } else {
        _temp_metadata_file->print(",%s", _opt_name[i]);
      }
    }
  }
  _temp_metadata_file->cr();
  // write jvm info
  _temp_metadata_file->print_cr("%s%s", _identifier_name[VMVersion], _vm_version);

  // write image info
  const char *image_ident = QuickStart::image_id();
  if (image_ident != NULL) {
    _temp_metadata_file->print_cr("%s%s", _identifier_name[ContainerImageID], image_ident);
  } else {
    _temp_metadata_file->print_cr("%s", _identifier_name[ContainerImageID]);
  }

  int cp_len = strlen(Arguments::get_appclasspath());
  _temp_metadata_file->print_cr("%s%d", _identifier_name[ClassPathLength], cp_len);
  _temp_metadata_file->write(Arguments::get_appclasspath(), cp_len);
  _temp_metadata_file->cr();

  _temp_metadata_file->print_cr("%s%d", _identifier_name[JVMOptionCount], _jvm_option_count);
  // write options args
  for (int index = 0; index < _jvm_option_count; index++) {
    const JavaVMOption *option = options_args->options + index;
    _temp_metadata_file->print_cr("%s", option->optionString);
  }

  _temp_metadata_file->flush();
  return true;
}

