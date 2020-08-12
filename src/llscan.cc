#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cinttypes>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <lldb/API/SBExpressionOptions.h>

#include "deps/rang/include/rang.hpp"
#include "src/error.h"
#include "src/llscan.h"
#include "src/llv8-inl.h"
#include "src/settings.h"

namespace llnode {

using lldb::ByteOrder;
using lldb::eReturnStatusFailed;
using lldb::eReturnStatusSuccessFinishResult;
using lldb::SBCommandReturnObject;
using lldb::SBDebugger;
using lldb::SBError;
using lldb::SBExpressionOptions;
using lldb::SBStream;
using lldb::SBTarget;
using lldb::SBValue;


char** ParsePrinterOptions(char** cmd, Printer::PrinterOptions* options) {
  static struct option opts[] = {
      {"full-string", no_argument, nullptr, 'F'},
      {"string-length", required_argument, nullptr, 'l'},
      {"array-length", required_argument, nullptr, 'l'},
      {"length", required_argument, nullptr, 'l'},
      {"print-map", no_argument, nullptr, 'm'},
      {"print-source", no_argument, nullptr, 's'},
      {"verbose", no_argument, nullptr, 'v'},
      {"detailed", no_argument, nullptr, 'd'},
      {"output-limit", required_argument, nullptr, 'n'},
      {nullptr, 0, nullptr, 0}};

  int argc = 1;
  for (char** p = cmd; p != nullptr && *p != nullptr; p++) argc++;

  char* args[argc];

  // Make this look like a command line, we need a valid element at index 0
  // for getopt_long to use in its error messages.
  char name[] = "llnode";
  args[0] = name;
  for (int i = 0; i < argc - 1; i++) args[i + 1] = cmd[i];

  // Reset getopts.
  optind = 0;
  opterr = 1;
  do {
    int arg = getopt_long(argc, args, "Fmsdvl:n:", opts, nullptr);
    if (arg == -1) break;

    switch (arg) {
      case 'F':
        options->length = 0;
        break;
      case 'm':
        options->print_map = true;
        break;
      case 'l':
        options->length = strtol(optarg, nullptr, 10);
        break;
      case 's':
        options->print_source = true;
        break;
      case 'd':
      case 'v':
        options->detailed = true;
        break;
      case 'n': {
        int limit = strtol(optarg, nullptr, 10);
        options->output_limit = limit && limit > 0 ? limit : 0;
      } break;
      default:
        continue;
    }
  } while (true);

  // Use the original cmd array for our return value.
  return &cmd[optind - 1];
}

bool FindObjectsCmd::DoExecute(SBDebugger d, char** cmd,
                               SBCommandReturnObject& result) {
  SBTarget target = d.GetSelectedTarget();
  if (!target.IsValid()) {
    result.SetError("No valid process, please start something\n");
    return false;
  }

  // Load V8 constants from postmortem data
  llscan_->v8()->Load(target);

  /* Ensure we have a map of objects. */
  if (!llscan_->ScanHeapForObjects(target, result)) {
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  Printer::PrinterOptions printer_options;
  ParsePrinterOptions(cmd, &printer_options);

  if (printer_options.detailed) {
    DetailedOutput(result);
  } else {
    SimpleOutput(result);
  }

  result.SetStatus(eReturnStatusSuccessFinishResult);
  return true;
}


void FindObjectsCmd::SimpleOutput(SBCommandReturnObject& result) {
  /* Create a vector to hold the entries sorted by instance count
   * TODO(hhellyer) - Make sort type an option (by count, size or name)
   */
  std::vector<TypeRecord*> sorted_by_count;
  TypeRecordMap::iterator end = llscan_->GetMapsToInstances().end();
  for (TypeRecordMap::iterator it = llscan_->GetMapsToInstances().begin();
       it != end; ++it) {
    sorted_by_count.push_back(it->second);
  }

  std::sort(sorted_by_count.begin(), sorted_by_count.end(),
            TypeRecord::CompareInstanceCounts);

  uint64_t total_objects = 0;
  uint64_t total_size = 0;

  result.Printf(" Instances  Total Size Name\n");
  result.Printf(" ---------- ---------- ----\n");

  for (std::vector<TypeRecord*>::iterator it = sorted_by_count.begin();
       it != sorted_by_count.end(); ++it) {
    TypeRecord* t = *it;
    result.Printf(" %10" PRId64 " %10" PRId64 " %s\n", t->GetInstanceCount(),
                  t->GetTotalInstanceSize(), t->GetTypeName().c_str());
    total_objects += t->GetInstanceCount();
    total_size += t->GetTotalInstanceSize();
  }

  result.Printf(" ---------- ---------- \n");
  result.Printf(" %10" PRId64 " %10" PRId64 " \n", total_objects, total_size);
}


void FindObjectsCmd::DetailedOutput(SBCommandReturnObject& result) {
  std::vector<DetailedTypeRecord*> sorted_by_count;
  for (auto kv : llscan_->GetDetailedMapsToInstances()) {
    sorted_by_count.push_back(kv.second);
  }

  std::sort(sorted_by_count.begin(), sorted_by_count.end(),
            TypeRecord::CompareInstanceCounts);
  uint64_t total_objects = 0;
  uint64_t total_size = 0;

  result.Printf(
      "   Sample Obj.  Instances  Total Size  Properties  Elements  Name\n");
  result.Printf(
      " ------------- ---------- ----------- ----------- --------- -----\n");

  for (auto t : sorted_by_count) {
    result.Printf(" %13" PRIx64 " %10" PRId64 " %11" PRId64 " %11" PRId64
                  " %9" PRId64 " %s\n",
                  *(t->GetInstances().begin()), t->GetInstanceCount(),
                  t->GetTotalInstanceSize(), t->GetOwnDescriptorsCount(),
                  t->GetIndexedPropertiesCount(), t->GetTypeName().c_str());
    total_objects += t->GetInstanceCount();
    total_size += t->GetTotalInstanceSize();
  }

  result.Printf(
      " ------------ ---------- ----------- ----------- ----------- ----\n");
  result.Printf("             %11" PRId64 " %11" PRId64 " \n", total_objects,
                total_size);
}


bool FindInstancesCmd::DoExecute(SBDebugger d, char** cmd,
                                 SBCommandReturnObject& result) {
  if (cmd == nullptr || *cmd == nullptr) {
    result.SetError("USAGE: v8 findjsinstances [flags] instance_name\n");
    return false;
  }

  SBTarget target = d.GetSelectedTarget();
  if (!target.IsValid()) {
    result.SetError("No valid process, please start something\n");
    return false;
  }

  // Load V8 constants from postmortem data
  llscan_->v8()->Load(target);

  /* Ensure we have a map of objects. */
  if (!llscan_->ScanHeapForObjects(target, result)) {
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  Printer::PrinterOptions printer_options;

  printer_options.detailed = detailed_;

  // Use same options as inspect?
  char** start = ParsePrinterOptions(cmd, &printer_options);

  std::string full_cmd;
  for (; start != nullptr && *start != nullptr; start++) full_cmd += *start;

  std::string type_name = full_cmd;

  TypeRecordMap::iterator instance_it =
      llscan_->GetMapsToInstances().find(type_name);

  if (instance_it != llscan_->GetMapsToInstances().end()) {
    TypeRecord* t = instance_it->second;

    // Update pagination options
    if (full_cmd != pagination_.command ||
        printer_options.output_limit != pagination_.output_limit) {
      pagination_.total_entries = t->GetInstanceCount();
      pagination_.command = full_cmd;
      pagination_.current_page = 0;
      pagination_.output_limit = printer_options.output_limit;
    } else {
      if (pagination_.output_limit <= 0 ||
          (pagination_.current_page + 1) * pagination_.output_limit >
              pagination_.total_entries) {
        pagination_.current_page = 0;
      } else {
        pagination_.current_page++;
      }
    }

    int initial_p_offset =
        (pagination_.current_page * printer_options.output_limit);
    int final_p_offset =
        initial_p_offset +
        std::min(pagination_.output_limit,
                 pagination_.total_entries -
                     pagination_.current_page * pagination_.output_limit);
    if (final_p_offset <= 0) {
      final_p_offset = pagination_.total_entries;
    }

    auto it = pagination_.current_page == 0
                  ? t->GetInstances().begin()
                  : std::next(t->GetInstances().begin(), initial_p_offset);
    for (; it != t->GetInstances().end() &&
           it != (std::next(t->GetInstances().begin(), final_p_offset));
         ++it) {
      Error err;
      v8::Value v8_value(llscan_->v8(), *it);
      Printer printer(llscan_->v8(), printer_options);
      std::string res = printer.Stringify(v8_value, err);
      result.Printf("%s\n", res.c_str());
    }
    if (it != t->GetInstances().end()) {
      result.Printf("..........\n");
    }
    result.Printf("(Showing %d to %d of %d instances)\n", initial_p_offset + 1,
                  final_p_offset, pagination_.total_entries);

  } else {
    // "No objects found with type name %s", type_name
    std::stringstream ss;
    ss << rang::style::bold << rang::fg::red
       << "No objects found with type name " << type_name << rang::fg::reset
       << rang::style::reset << std::endl;
    std::string str(ss.str());
    result.Printf("%s", str.c_str());
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  result.SetStatus(eReturnStatusSuccessFinishResult);
  return true;
}


bool NodeInfoCmd::DoExecute(SBDebugger d, char** cmd,
                            SBCommandReturnObject& result) {
  SBTarget target = d.GetSelectedTarget();
  if (!target.IsValid()) {
    result.SetError("No valid process, please start something\n");
    return false;
  }

  // Load V8 constants from postmortem data
  llscan_->v8()->Load(target);

  /* Ensure we have a map of objects. */
  if (!llscan_->ScanHeapForObjects(target, result)) {
    return false;
  }

  std::string process_type_name("process");

  TypeRecordMap::iterator instance_it =
      llscan_->GetMapsToInstances().find(process_type_name);

  if (instance_it != llscan_->GetMapsToInstances().end()) {
    TypeRecord* t = instance_it->second;
    for (auto it : t->GetInstances()) {
      Error err;

      // The properties object should be a JSObject
      v8::JSObject process_obj(llscan_->v8(), it);


      v8::Value pid_val = process_obj.GetProperty("pid", err);

      if (pid_val.v8() != nullptr) {
        v8::Smi pid_smi(pid_val);
        result.Printf("Information for process id %" PRId64
                      " (process=0x%" PRIx64 ")\n",
                      pid_smi.GetValue(), process_obj.raw());
      } else {
        // This isn't the process object we are looking for.
        continue;
      }

      v8::Value platform_val = process_obj.GetProperty("platform", err);

      if (platform_val.v8() != nullptr) {
        v8::String platform_str(platform_val);
        result.Printf("Platform = %s, ", platform_str.ToString(err).c_str());
      }

      v8::Value arch_val = process_obj.GetProperty("arch", err);

      if (arch_val.v8() != nullptr) {
        v8::String arch_str(arch_val);
        result.Printf("Architecture = %s, ", arch_str.ToString(err).c_str());
      }

      v8::Value ver_val = process_obj.GetProperty("version", err);

      if (ver_val.v8() != nullptr) {
        v8::String ver_str(ver_val);
        result.Printf("Node Version = %s\n", ver_str.ToString(err).c_str());
      }

      // Note the extra s on versions!
      v8::Value versions_val = process_obj.GetProperty("versions", err);
      if (versions_val.v8() != nullptr) {
        v8::JSObject versions_obj(versions_val);

        std::vector<std::string> version_keys;

        // Get the list of keys on an object as strings.
        versions_obj.Keys(version_keys, err);

        std::sort(version_keys.begin(), version_keys.end());

        result.Printf("Component versions (process.versions=0x%" PRIx64 "):\n",
                      versions_val.raw());

        for (std::vector<std::string>::iterator key = version_keys.begin();
             key != version_keys.end(); ++key) {
          v8::Value ver_val = versions_obj.GetProperty(*key, err);
          if (ver_val.v8() != nullptr) {
            v8::String ver_str(ver_val);
            result.Printf("    %s = %s\n", key->c_str(),
                          ver_str.ToString(err).c_str());
          }
        }
      }

      v8::Value release_val = process_obj.GetProperty("release", err);
      if (release_val.v8() != nullptr) {
        v8::JSObject release_obj(release_val);

        std::vector<std::string> release_keys;

        // Get the list of keys on an object as strings.
        release_obj.Keys(release_keys, err);

        result.Printf("Release Info (process.release=0x%" PRIx64 "):\n",
                      release_val.raw());

        for (std::vector<std::string>::iterator key = release_keys.begin();
             key != release_keys.end(); ++key) {
          v8::Value ver_val = release_obj.GetProperty(*key, err);
          if (ver_val.v8() != nullptr) {
            v8::String ver_str(ver_val);
            result.Printf("    %s = %s\n", key->c_str(),
                          ver_str.ToString(err).c_str());
          }
        }
      }

      v8::Value execPath_val = process_obj.GetProperty("execPath", err);

      if (execPath_val.v8() != nullptr) {
        v8::String execPath_str(execPath_val);
        result.Printf("Executable Path = %s\n",
                      execPath_str.ToString(err).c_str());
      }

      v8::Value argv_val = process_obj.GetProperty("argv", err);

      if (argv_val.v8() != nullptr) {
        v8::JSArray argv_arr(argv_val);
        result.Printf("Command line arguments (process.argv=0x%" PRIx64 "):\n",
                      argv_val.raw());
        // argv is an array, which we can treat as a subtype of object.
        int64_t length = argv_arr.GetArrayLength(err);
        for (int64_t i = 0; i < length; ++i) {
          v8::Value element_val = argv_arr.GetArrayElement(i, err);
          if (element_val.v8() != nullptr) {
            v8::String element_str(element_val);
            result.Printf("    [%" PRId64 "] = '%s'\n", i,
                          element_str.ToString(err).c_str());
          }
        }
      }

      /* The docs for process.execArgv say "These options are useful in order
       * to spawn child processes with the same execution environment
       * as the parent." so being able to check these have been passed in
       * seems like a good idea.
       */
      v8::Value execArgv_val = process_obj.GetProperty("execArgv", err);

      if (argv_val.v8() != nullptr) {
        // Should possibly just treat this as an object in case anyone has
        // attached a property.
        v8::JSArray execArgv_arr(execArgv_val);
        result.Printf(
            "Node.js Command line arguments (process.execArgv=0x%" PRIx64
            "):\n",
            execArgv_val.raw());
        // execArgv is an array, which we can treat as a subtype of object.
        int64_t length = execArgv_arr.GetArrayLength(err);
        for (int64_t i = 0; i < length; ++i) {
          v8::Value element_val = execArgv_arr.GetArrayElement(i, err);
          if (element_val.v8() != nullptr) {
            v8::String element_str(element_val);
            result.Printf("    [%" PRId64 "] = '%s'\n", i,
                          element_str.ToString(err).c_str());
          }
        }
      }
    }

  } else {
    result.Printf("No process objects found.\n");
  }

  return true;
}

bool FindReferencesCmd::DoExecute(SBDebugger d, char** cmd,
                                  SBCommandReturnObject& result) {
  if (cmd == nullptr || *cmd == nullptr) {
    result.SetError("USAGE: v8 findrefs expr\n");
    return false;
  }

  SBTarget target = d.GetSelectedTarget();
  if (!target.IsValid()) {
    result.SetError("No valid process, please start something\n");
    return false;
  }

  // Load V8 constants from postmortem data
  llscan_->v8()->Load(target);

  ScanOptions scan_options;
  char** start = ParseScanOptions(cmd, &scan_options);

  if (*start == nullptr) {
    result.SetError("Missing search parameter");
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  ObjectScanner* scanner;

  switch (scan_options.scan_type) {
    case ScanOptions::ScanType::kFieldValue: {
      std::string full_cmd;
      for (; start != nullptr && *start != nullptr; start++) full_cmd += *start;

      SBExpressionOptions options;
      SBValue value = target.EvaluateExpression(full_cmd.c_str(), options);
      if (value.GetError().Fail()) {
        SBError error = value.GetError();
        result.SetError(error);
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      // Check the address we've been given at least looks like a valid object.
      v8::Value search_value(llscan_->v8(), value.GetValueAsSigned());
      v8::Smi smi(search_value);
      if (smi.Check()) {
        result.SetError("Search value is an SMI.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      scanner = new ReferenceScanner(llscan_, search_value);
      break;
    }
    case ScanOptions::ScanType::kPropertyName: {
      // Check for extra parameters or parameters that needed quoting.
      if (start[1] != nullptr) {
        result.SetError("Extra search parameter or unquoted string specified.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      std::string property_name = start[0];
      scanner = new PropertyScanner(llscan_, property_name);
      break;
    }
    case ScanOptions::ScanType::kStringValue: {
      // Check for extra parameters or parameters that needed quoting.
      if (start[1] != nullptr) {
        result.SetError("Extra search parameter or unquoted string specified.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      std::string string_value = start[0];
      scanner = new StringScanner(llscan_, string_value);
      break;
    }
    /* We can add options to the command and further sub-classes of
     * object scanner to do other searches, e.g.:
     * - Objects that refer to a particular string literal.
     *   (lldb) findreferences -s "Hello World!"
     */
    case ScanOptions::ScanType::kBadOption: {
      result.SetError("Invalid search type");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }
  }

  /* Ensure we have a map of objects.
   * (Do this after we've checked the options to avoid
   * a long pause before reporting an error.)
   */
  if (!llscan_->ScanHeapForObjects(target, result)) {
    delete scanner;
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  if (!scanner->AreReferencesLoaded()) {
    ScanForReferences(scanner);
  }

  // If we're using recursive findrefs, we have to make sure the
  // RecursiveScanner is initialized as well.
  if (scan_options.recursive_scan) {
    auto ref_scanner = new ReferenceScanner(llscan_, v8::Value());
    if (!ref_scanner->AreReferencesLoaded()) {
      ScanForReferences(ref_scanner);
    }
  }

  // Store already visited references to avoid and infinite recursive loop
  // when `--recursive (-r)` option is set
  ReferencesVector already_visited_references;

  // Get the list of references for the given search value, property or string
  ReferencesVector* references = scanner->GetReferences();
  PrintReferences(result, references, scanner, &scan_options,
                  &already_visited_references);

  delete scanner;

  result.SetStatus(eReturnStatusSuccessFinishResult);
  return true;
}


void FindReferencesCmd::ScanForReferences(ObjectScanner* scanner) {
  // Walk all the object instances and handle them according to their type.
  TypeRecordMap mapstoinstances = llscan_->GetMapsToInstances();
  for (auto const entry : mapstoinstances) {
    TypeRecord* typerecord = entry.second;

    for (uint64_t addr : typerecord->GetInstances()) {
      Error err;
      v8::Value obj_value(llscan_->v8(), addr);
      v8::HeapObject heap_object(obj_value);
      int64_t type = heap_object.GetType(err);
      v8::LLV8* v8 = heap_object.v8();

      // We only need to handle the types that are in
      // FindJSObjectsVisitor::IsAHistogramType
      // as those are the only objects that end up in GetMapsToInstances
      if (v8::JSObject::IsObjectType(v8, type) ||
          type == v8->types()->kJSArrayType) {
        // Objects can have elements and arrays can have named properties.
        // Basically we need to access objects and arrays as both objects and
        // arrays.
        v8::JSObject js_obj(heap_object);
        scanner->ScanRefs(js_obj, err);

      } else if (type < v8->types()->kFirstNonstringType) {
        v8::String str(heap_object);
        scanner->ScanRefs(str, err);

      } else if (type == v8->types()->kJSTypedArrayType) {
        // These should only point to off heap memory,
        // this case should be a no-op.
      } else {
        // result.Printf("Unhandled type: %" PRId64 " for addr %" PRIx64
        //    "\n", type, addr);
      }
    }
  }
}

void FindReferencesCmd::PrintRecursiveReferences(
    lldb::SBCommandReturnObject& result, ScanOptions* options,
    ReferencesVector* visited_references, uint64_t address, int level) {
  Settings* settings = Settings::GetSettings();
  unsigned int padding = settings->GetTreePadding();

  std::string branch = std::string(padding * level, ' ') + "+ ";

  result.Printf("%s", branch.c_str());

  if (find(visited_references->begin(), visited_references->end(), address) !=
      visited_references->end()) {
    std::stringstream seen_str;
    seen_str << rang::fg::red << " [seen above]" << rang::fg::reset
             << std::endl;
    result.Printf("%s", seen_str.str().c_str());
  } else {
    visited_references->push_back(address);
    v8::Value value(llscan_->v8(), address);
    ReferenceScanner scanner_(llscan_, value);
    ReferencesVector* references_ = scanner_.GetReferences();
    PrintReferences(result, references_, &scanner_, options, visited_references,
                    level + 1);
  }
}

void FindReferencesCmd::PrintReferences(
    SBCommandReturnObject& result, ReferencesVector* references,
    ObjectScanner* scanner, ScanOptions* options,
    ReferencesVector* already_visited_references, int level) {
  // Walk all the object instances and handle them according to their type.
  TypeRecordMap mapstoinstances = llscan_->GetMapsToInstances();

  for (uint64_t addr : *references) {
    Error err;
    v8::Value obj_value(llscan_->v8(), addr);
    v8::HeapObject heap_object(obj_value);
    int64_t type = heap_object.GetType(err);
    v8::LLV8* v8 = heap_object.v8();

    // We only need to handle the types that are in
    // FindJSObjectsVisitor::IsAHistogramType
    // as those are the only objects that end up in GetMapsToInstances
    if (v8::JSObject::IsObjectType(v8, type) ||
        type == v8->types()->kJSArrayType) {
      // Objects can have elements and arrays can have named properties.
      // Basically we need to access objects and arrays as both objects and
      // arrays.
      v8::JSObject js_obj(heap_object);
      scanner->PrintRefs(result, js_obj, err, level);

      if (options->recursive_scan) {
        PrintRecursiveReferences(result, options, already_visited_references,
                                 addr, level);
      }

    } else if (type < v8->types()->kFirstNonstringType) {
      v8::String str(heap_object);
      scanner->PrintRefs(result, str, err, level);

      if (options->recursive_scan) {
        PrintRecursiveReferences(result, options, already_visited_references,
                                 addr, level);
      }

    } else if (type == v8->types()->kJSTypedArrayType) {
      // These should only point to off heap memory,
      // this case should be a no-op.
    } else {
      // result.Printf("Unhandled type: %" PRId64 " for addr %" PRIx64
      //    "\n", type, addr);
    }
  }

  // Print references found directly inside Context objects
  Error err;
  scanner->PrintContextRefs(result, err, this, options,
                            already_visited_references, level);
}


char** FindReferencesCmd::ParseScanOptions(char** cmd, ScanOptions* options) {
  static struct option opts[] = {{"value", no_argument, nullptr, 'v'},
                                 {"name", no_argument, nullptr, 'n'},
                                 {"string", no_argument, nullptr, 's'},
                                 {"recursive", no_argument, nullptr, 'r'},
                                 {nullptr, 0, nullptr, 0}};

  int argc = 1;
  for (char** p = cmd; p != nullptr && *p != nullptr; p++) argc++;

  char* args[argc];

  // Make this look like a command line, we need a valid element at index 0
  // for getopt_long to use in its error messages.
  char name[] = "llscan";
  args[0] = name;
  for (int i = 0; i < argc - 1; i++) args[i + 1] = cmd[i];

  bool found_scan_type = false;

  // Reset getopts.
  optind = 0;
  opterr = 1;
  do {
    int arg = getopt_long(argc, args, "vnsr", opts, nullptr);
    if (arg == -1) break;

    if (found_scan_type) {
      options->scan_type = ScanOptions::ScanType::kBadOption;
      break;
    }

    switch (arg) {
      case 'r':
        options->recursive_scan = true;
        break;
      case 'v':
        options->scan_type = ScanOptions::ScanType::kFieldValue;
        found_scan_type = true;
        break;
      case 'n':
        options->scan_type = ScanOptions::ScanType::kPropertyName;
        found_scan_type = true;
        break;
      case 's':
        options->scan_type = ScanOptions::ScanType::kStringValue;
        found_scan_type = true;
        break;
      default:
        options->scan_type = ScanOptions::ScanType::kBadOption;
        break;
    }
  } while (true);

  return &cmd[optind - 1];
}

// Walk all contexts previously stored and print search_value_
// reference if it exists. Not all values are associated with
// a context object. It seems that Function-Local variables are
// stored in the stack, and when some nested closure references
// it is allocated in a Context object.
void FindReferencesCmd::ReferenceScanner::PrintContextRefs(
    SBCommandReturnObject& result, Error& err, FindReferencesCmd* cli_cmd_,
    ScanOptions* options, ReferencesVector* already_visited_references,
    int level) {
  ContextVector* contexts = llscan_->GetContexts();
  v8::LLV8* v8 = llscan_->v8();

  for (auto ctx : *contexts) {
    Error err;
    v8::HeapObject context_obj(v8, ctx);
    v8::Context c(context_obj);

    v8::Context::Locals locals(&c, err);
    // If we can't read locals in this context, just go to the next.
    if (err.Fail()) continue;

    for (v8::Context::Locals::Iterator it = locals.begin(); it != locals.end();
         it++) {
      if ((*it).raw() == search_value_.raw()) {
        std::string name = "???";
        v8::String _name = it.LocalName(err);
        // TODO(mmarchini): use Check once #294 gets merged
        if (err.Success()) {
          std::string maybe_name = _name.ToString(err);
          if (err.Success())
            name = maybe_name;
          else
            PRINT_DEBUG("Couldn't get the variable name for 0x%" PRIx64
                        " in context 0x%" PRIx64,
                        search_value_.raw(), c.raw());
        }

        std::stringstream ss;
        ss << rang::fg::cyan << "0x%" PRIx64 << rang::fg::reset << ": "
           << rang::fg::magenta << "Context" << rang::style::bold
           << rang::fg::yellow << ".%s" << rang::fg::reset << rang::style::reset
           << "=" << rang::fg::cyan << "0x%" PRIx64 << rang::fg::reset << "\n";

        result.Printf(ss.str().c_str(), c.raw(), name.c_str(),
                      search_value_.raw());

        if (options->recursive_scan) {
          cli_cmd_->PrintRecursiveReferences(
              result, options, already_visited_references, c.raw(), level);
        }
      }
    }
  }
}

std::string FindReferencesCmd::ObjectScanner::GetPropertyReferenceString(
    int level) {
  std::stringstream ss;
  ss << rang::fg::cyan << "0x%" PRIx64 << rang::fg::reset << ": "
     << rang::fg::magenta << "%s" << rang::style::bold << rang::fg::yellow
     << ".%s" << rang::fg::reset << rang::style::reset << "=" << rang::fg::cyan
     << "0x%" PRIx64 << rang::fg::reset << "\n";
  return ss.str();
}

std::string FindReferencesCmd::ObjectScanner::GetArrayReferenceString(
    int level) {
  std::stringstream ss;
  ss << rang::fg::cyan << "0x%" PRIx64 << rang::fg::reset << ": "
     << rang::fg::magenta << "%s" << rang::style::bold << rang::fg::yellow
     << "[%" PRId64 "]" << rang::fg::reset << rang::style::reset << "="
     << rang::fg::cyan << "0x%" PRIx64 << rang::fg::reset << "\n";
  return ss.str();
}


void FindReferencesCmd::ReferenceScanner::PrintRefs(
    SBCommandReturnObject& result, v8::JSObject& js_obj, Error& err,
    int level) {
  int64_t length = js_obj.GetArrayLength(err);
  for (int64_t i = 0; i < length; ++i) {
    v8::Value v = js_obj.GetArrayElement(i, err);

    // Array is borked, or not array at all - skip it
    if (!err.Success()) break;

    if (v.raw() != search_value_.raw()) continue;

    std::string type_name = js_obj.GetTypeName(err);

    std::string reference_template(GetArrayReferenceString(level));
    result.Printf(reference_template.c_str(), js_obj.raw(), type_name.c_str(),
                  i, search_value_.raw());
  }

  // Walk all the properties in this object.
  // We only create strings for the field names that match the search
  // value.
  std::vector<std::pair<v8::Value, v8::Value>> entries = js_obj.Entries(err);
  if (err.Fail()) {
    return;
  }
  for (auto entry : entries) {
    v8::Value v = entry.second;
    if (v.raw() == search_value_.raw()) {
      std::string key = entry.first.ToString(err);
      std::string type_name = js_obj.GetTypeName(err);

      std::string reference_template(GetPropertyReferenceString(level));

      result.Printf(reference_template.c_str(), js_obj.raw(), type_name.c_str(),
                    key.c_str(), search_value_.raw());
    }
  }
}


void FindReferencesCmd::ReferenceScanner::PrintRefs(
    SBCommandReturnObject& result, v8::String& str, Error& err, int level) {
  v8::LLV8* v8 = str.v8();

  v8::CheckedType<int64_t> repr = str.Representation(err);
  RETURN_IF_INVALID(repr, );

  // Concatenated and sliced strings refer to other strings so
  // we need to check their references.
  if (*repr == v8->string()->kSlicedStringTag) {
    v8::SlicedString sliced_str(str);
    v8::String parent = sliced_str.Parent(err);
    if (err.Success() && parent.raw() == search_value_.raw()) {
      std::string type_name = sliced_str.GetTypeName(err);

      std::string reference_template(GetPropertyReferenceString(level));
      result.Printf(reference_template.c_str(), str.raw(), type_name.c_str(),
                    "<Parent>", search_value_.raw());
    }
  } else if (*repr == v8->string()->kConsStringTag) {
    v8::ConsString cons_str(str);

    v8::String first = cons_str.First(err);
    if (err.Success() && first.raw() == search_value_.raw()) {
      std::string type_name = cons_str.GetTypeName(err);

      std::string reference_template(GetPropertyReferenceString(level));
      result.Printf(reference_template.c_str(), str.raw(), type_name.c_str(),
                    "<First>", search_value_.raw());
    }

    v8::String second = cons_str.Second(err);
    if (err.Success() && second.raw() == search_value_.raw()) {
      std::string type_name = cons_str.GetTypeName(err);

      std::string reference_template(GetPropertyReferenceString(level));
      result.Printf(reference_template.c_str(), str.raw(), type_name.c_str(),
                    "<Second>", search_value_.raw());
    }
  } else if (*repr == v8->string()->kThinStringTag) {
    v8::ThinString thin_str(str);
    v8::String actual = thin_str.Actual(err);
    if (err.Success() && actual.raw() == search_value_.raw()) {
      std::string type_name = thin_str.GetTypeName(err);

      std::string reference_template(GetPropertyReferenceString(level));
      result.Printf(reference_template.c_str(), str.raw(), type_name.c_str(),
                    "<Actual>", search_value_.raw());
    }
  }
  // Nothing to do for other kinds of string.
}


void FindReferencesCmd::ReferenceScanner::ScanRefs(v8::JSObject& js_obj,
                                                   Error& err) {
  ReferencesVector* references;
  std::set<uint64_t> already_saved;

  int64_t length = js_obj.GetArrayLength(err);
  for (int64_t i = 0; i < length; ++i) {
    v8::Value v = js_obj.GetArrayElement(i, err);

    // Array is borked, or not array at all - skip it
    if (!err.Success()) break;
    if (already_saved.count(v.raw())) continue;

    references = llscan_->GetReferencesByValue(v.raw());
    references->push_back(js_obj.raw());
    already_saved.insert(v.raw());
  }

  // Walk all the properties in this object.
  // We only create strings for the field names that match the search
  // value.
  std::vector<std::pair<v8::Value, v8::Value>> entries = js_obj.Entries(err);
  if (err.Fail()) {
    return;
  }
  for (auto entry : entries) {
    v8::Value v = entry.second;

    if (already_saved.count(v.raw())) continue;

    references = llscan_->GetReferencesByValue(v.raw());
    references->push_back(js_obj.raw());
    already_saved.insert(v.raw());
  }
}


void FindReferencesCmd::ReferenceScanner::ScanRefs(v8::String& str,
                                                   Error& err) {
  ReferencesVector* references;
  std::set<uint64_t> already_saved;

  v8::LLV8* v8 = str.v8();

  v8::CheckedType<int64_t> repr = str.Representation(err);
  RETURN_IF_INVALID(repr, );

  // Concatenated and sliced strings refer to other strings so
  // we need to check their references.

  if (*repr == v8->string()->kSlicedStringTag) {
    v8::SlicedString sliced_str(str);
    v8::String parent = sliced_str.Parent(err);

    if (err.Success()) {
      references = llscan_->GetReferencesByValue(parent.raw());
      references->push_back(str.raw());
    }

  } else if (*repr == v8->string()->kConsStringTag) {
    v8::ConsString cons_str(str);

    v8::String first = cons_str.First(err);
    if (err.Success()) {
      references = llscan_->GetReferencesByValue(first.raw());
      references->push_back(str.raw());
    }

    v8::String second = cons_str.Second(err);
    if (err.Success() && first.raw() != second.raw()) {
      references = llscan_->GetReferencesByValue(second.raw());
      references->push_back(str.raw());
    }
  } else if (*repr == v8->string()->kThinStringTag) {
    v8::ThinString thin_str(str);
    v8::String actual = thin_str.Actual(err);

    if (err.Success()) {
      references = llscan_->GetReferencesByValue(actual.raw());
      references->push_back(str.raw());
    }
  }
  // Nothing to do for other kinds of string.
}


bool FindReferencesCmd::ReferenceScanner::AreReferencesLoaded() {
  return llscan_->AreReferencesByValueLoaded();
}


ReferencesVector* FindReferencesCmd::ReferenceScanner::GetReferences() {
  return llscan_->GetReferencesByValue(search_value_.raw());
}


void FindReferencesCmd::PropertyScanner::PrintRefs(
    SBCommandReturnObject& result, v8::JSObject& js_obj, Error& err,
    int level) {
  // (Note: We skip array elements as they don't have names.)

  // Walk all the properties in this object.
  // We only create strings for the field names that match the search
  // value.
  std::vector<std::pair<v8::Value, v8::Value>> entries = js_obj.Entries(err);
  if (err.Fail()) {
    return;
  }
  for (auto entry : entries) {
    v8::HeapObject nameObj(entry.first);
    std::string key = entry.first.ToString(err);
    if (err.Fail()) {
      continue;
    }
    if (key == search_value_) {
      std::string type_name = js_obj.GetTypeName(err);

      std::string reference_template(GetPropertyReferenceString());
      result.Printf(reference_template.c_str(), js_obj.raw(), type_name.c_str(),
                    key.c_str(), entry.second.raw());
    }
  }
}


void FindReferencesCmd::PropertyScanner::ScanRefs(v8::JSObject& js_obj,
                                                  Error& err) {
  // (Note: We skip array elements as they don't have names.)

  // Walk all the properties in this object.
  // We only create strings for the field names that match the search
  // value.
  ReferencesVector* references;
  std::vector<std::pair<v8::Value, v8::Value>> entries = js_obj.Entries(err);
  if (err.Fail()) {
    return;
  }
  for (auto entry : entries) {
    v8::HeapObject nameObj(entry.first);
    std::string key = entry.first.ToString(err);
    if (err.Fail()) {
      continue;
    }
    references = llscan_->GetReferencesByProperty(key);
    references->push_back(js_obj.raw());
  }
}


bool FindReferencesCmd::PropertyScanner::AreReferencesLoaded() {
  return llscan_->AreReferencesByPropertyLoaded();
}


ReferencesVector* FindReferencesCmd::PropertyScanner::GetReferences() {
  return llscan_->GetReferencesByProperty(search_value_);
}


void FindReferencesCmd::StringScanner::PrintRefs(SBCommandReturnObject& result,
                                                 v8::JSObject& js_obj,
                                                 Error& err, int level) {
  v8::LLV8* v8 = js_obj.v8();

  int64_t length = js_obj.GetArrayLength(err);
  for (int64_t i = 0; i < length; ++i) {
    v8::Value v = js_obj.GetArrayElement(i, err);
    if (err.Fail()) {
      continue;
    }
    v8::HeapObject valueObj(v);

    int64_t type = valueObj.GetType(err);
    if (err.Fail()) {
      continue;
    }
    if (type < v8->types()->kFirstNonstringType) {
      v8::String valueString(valueObj);
      std::string value = valueString.ToString(err);
      if (err.Fail()) {
        continue;
      }
      if (err.Success() && search_value_ == value) {
        std::string type_name = js_obj.GetTypeName(err);

        std::stringstream ss;
        ss << rang::fg::cyan << std::hex << js_obj.raw() << std::dec
           << rang::fg::reset;

        result.Printf("%s: %s[%" PRId64 "]=0x%" PRIx64 " '%s'\n",
                      ss.str().c_str(), type_name.c_str(), i, v.raw(),
                      value.c_str());
      }
    }
  }

  // Walk all the properties in this object.
  // We only create strings for the field names that match the search
  // value.
  std::vector<std::pair<v8::Value, v8::Value>> entries = js_obj.Entries(err);
  if (err.Success()) {
    for (auto entry : entries) {
      v8::HeapObject valueObj(entry.second);
      int64_t type = valueObj.GetType(err);
      if (err.Fail()) {
        continue;
      }
      if (type < v8->types()->kFirstNonstringType) {
        v8::String valueString(valueObj);
        std::string value = valueString.ToString(err);
        if (err.Fail()) {
          continue;
        }
        if (search_value_ == value) {
          std::string key = entry.first.ToString(err);
          if (err.Fail()) {
            continue;
          }
          std::string type_name = js_obj.GetTypeName(err);

          std::stringstream ss;
          ss << rang::fg::cyan << "0x" << std::hex << js_obj.raw() << std::dec
             << rang::fg::reset << ": " << type_name.c_str() << "."
             << key.c_str() << "=" << rang::fg::cyan << "0x" << std::hex
             << entry.second.raw() << std::dec << rang::fg::reset << " '"
             << value.c_str() << "'" << std::endl;

          result.Printf("%s", ss.str().c_str());
        }
      }
    }
  }
}


void FindReferencesCmd::StringScanner::PrintRefs(SBCommandReturnObject& result,
                                                 v8::String& str, Error& err,
                                                 int level) {
  v8::LLV8* v8 = str.v8();

  // Concatenated and sliced strings refer to other strings so
  // we need to check their references.

  v8::CheckedType<int64_t> repr = str.Representation(err);
  RETURN_IF_INVALID(repr, );

  if (*repr == v8->string()->kSlicedStringTag) {
    v8::SlicedString sliced_str(str);
    v8::String parent_str = sliced_str.Parent(err);
    if (err.Fail()) return;
    std::string parent = parent_str.ToString(err);
    if (err.Success() && search_value_ == parent) {
      std::string type_name = sliced_str.GetTypeName(err);
      result.Printf("0x%" PRIx64 ": %s.%s=0x%" PRIx64 " '%s'\n", str.raw(),
                    type_name.c_str(), "<Parent>", parent_str.raw(),
                    parent.c_str());
    }
  } else if (*repr == v8->string()->kConsStringTag) {
    v8::ConsString cons_str(str);

    v8::String first_str = cons_str.First(err);
    if (err.Fail()) return;

    // It looks like sometimes one of the strings can be <null> or another
    // value,
    // verify that they are a JavaScript String before calling ToString.
    int64_t first_type = first_str.GetType(err);
    if (err.Fail()) return;

    if (first_type < v8->types()->kFirstNonstringType) {
      std::string first = first_str.ToString(err);

      if (err.Success() && search_value_ == first) {
        std::string type_name = cons_str.GetTypeName(err);
        result.Printf("0x%" PRIx64 ": %s.%s=0x%" PRIx64 " '%s'\n", str.raw(),
                      type_name.c_str(), "<First>", first_str.raw(),
                      first.c_str());
      }
    }

    v8::String second_str = cons_str.Second(err);
    if (err.Fail()) return;

    // It looks like sometimes one of the strings can be <null> or another
    // value,
    // verify that they are a JavaScript String before calling ToString.
    int64_t second_type = second_str.GetType(err);
    if (err.Fail()) return;

    if (second_type < v8->types()->kFirstNonstringType) {
      std::string second = second_str.ToString(err);

      if (err.Success() && search_value_ == second) {
        std::string type_name = cons_str.GetTypeName(err);
        result.Printf("0x%" PRIx64 ": %s.%s=0x%" PRIx64 " '%s'\n", str.raw(),
                      type_name.c_str(), "<Second>", second_str.raw(),
                      second.c_str());
      }
    }
  }
  // Nothing to do for other kinds of string.
  // They are strings so we will find references to them.
}


void FindReferencesCmd::StringScanner::ScanRefs(v8::JSObject& js_obj,
                                                Error& err) {
  v8::LLV8* v8 = js_obj.v8();
  ReferencesVector* references;
  std::set<std::string> already_saved;

  int64_t length = js_obj.GetArrayLength(err);
  for (int64_t i = 0; i < length; ++i) {
    v8::Value v = js_obj.GetArrayElement(i, err);
    if (err.Fail()) {
      continue;
    }
    v8::HeapObject valueObj(v);

    int64_t type = valueObj.GetType(err);
    if (err.Fail()) {
      continue;
    }
    if (type < v8->types()->kFirstNonstringType) {
      v8::String valueString(valueObj);
      std::string value = valueString.ToString(err);
      if (err.Fail()) {
        continue;
      }

      if (already_saved.count(value)) continue;

      references = llscan_->GetReferencesByString(value);
      references->push_back(js_obj.raw());
      already_saved.insert(value);
    }
  }

  // Walk all the properties in this object.
  // We only create strings for the field names that match the search
  // value.
  std::vector<std::pair<v8::Value, v8::Value>> entries = js_obj.Entries(err);
  if (err.Success()) {
    for (auto entry : entries) {
      v8::HeapObject valueObj(entry.second);
      int64_t type = valueObj.GetType(err);
      if (err.Fail()) {
        continue;
      }
      if (type < v8->types()->kFirstNonstringType) {
        v8::String valueString(valueObj);
        std::string value = valueString.ToString(err);
        if (err.Fail()) {
          continue;
        }
        if (already_saved.count(value)) continue;

        references = llscan_->GetReferencesByString(value);
        references->push_back(js_obj.raw());
        already_saved.insert(value);
      }
    }
  }
}


void FindReferencesCmd::StringScanner::ScanRefs(v8::String& str, Error& err) {
  v8::LLV8* v8 = str.v8();
  ReferencesVector* references;

  // Concatenated and sliced strings refer to other strings so
  // we need to check their references.

  v8::CheckedType<int64_t> repr = str.Representation(err);
  RETURN_IF_INVALID(repr, );

  if (*repr == v8->string()->kSlicedStringTag) {
    v8::SlicedString sliced_str(str);
    v8::String parent_str = sliced_str.Parent(err);
    if (err.Fail()) return;
    std::string parent = parent_str.ToString(err);
    if (err.Success()) {
      references = llscan_->GetReferencesByString(parent);
      references->push_back(str.raw());
    }
  } else if (*repr == v8->string()->kConsStringTag) {
    v8::ConsString cons_str(str);

    v8::String first_str = cons_str.First(err);
    if (err.Fail()) return;

    // It looks like sometimes one of the strings can be <null> or another
    // value,
    // verify that they are a JavaScript String before calling ToString.
    int64_t first_type = first_str.GetType(err);
    if (err.Fail()) return;

    if (first_type < v8->types()->kFirstNonstringType) {
      std::string first = first_str.ToString(err);

      if (err.Success()) {
        references = llscan_->GetReferencesByString(first);
        references->push_back(str.raw());
      }
    }

    v8::String second_str = cons_str.Second(err);
    if (err.Fail()) return;

    // It looks like sometimes one of the strings can be <null> or another
    // value,
    // verify that they are a JavaScript String before calling ToString.
    int64_t second_type = second_str.GetType(err);
    if (err.Fail()) return;

    if (second_type < v8->types()->kFirstNonstringType) {
      std::string second = second_str.ToString(err);

      if (err.Success()) {
        references = llscan_->GetReferencesByString(second);
        references->push_back(str.raw());
      }
    }
  }
  // Nothing to do for other kinds of string.
  // They are strings so we will find references to them.
}


bool FindReferencesCmd::StringScanner::AreReferencesLoaded() {
  return llscan_->AreReferencesByStringLoaded();
}


ReferencesVector* FindReferencesCmd::StringScanner::GetReferences() {
  return llscan_->GetReferencesByString(search_value_);
}


FindJSObjectsVisitor::FindJSObjectsVisitor(SBTarget& target, LLScan* llscan)
    : target_(target), llscan_(llscan) {
  found_count_ = 0;
  address_byte_size_ = target_.GetProcess().GetAddressByteSize();
}


/* Visit every address, a bit brute force but it works. */
uint64_t FindJSObjectsVisitor::Visit(uint64_t location, uint64_t word) {
  v8::Value v8_value(llscan_->v8(), word);

  Error err;
  // Test if this is SMI
  // Skip inspecting things that look like Smi's, they aren't objects.
  v8::Smi smi(v8_value);
  if (smi.Check()) return address_byte_size_;

  v8::HeapObject heap_object(v8_value);
  if (!heap_object.Check()) return address_byte_size_;

  v8::HeapObject map_object = heap_object.GetMap(err);
  if (err.Fail() || !map_object.Check()) return address_byte_size_;

  v8::Map map(map_object);

  MapCacheEntry map_info;
  if (map_cache_.count(map.raw()) == 0) {
    map_info.Load(map, heap_object, llscan_->v8(), err);
    if (err.Fail()) {
      return address_byte_size_;
    }
    // Cache result
    map_cache_.emplace(map.raw(), map_info);
  } else {
    map_info = map_cache_.at(map.raw());
  }

  if (map_info.is_context) {
    InsertOnContexts(word, err);
    return address_byte_size_;
  }

  if (!map_info.is_histogram) return address_byte_size_;

  InsertOnMapsToInstances(word, map, map_info, err);
  InsertOnDetailedMapsToInstances(word, map, map_info, err);

  if (err.Fail()) {
    return address_byte_size_;
  }

  found_count_++;

  /* Just advance one word.
   * (Should advance by object size, assuming objects can't overlap!)
   */
  return address_byte_size_;
}

void FindJSObjectsVisitor::InsertOnContexts(uint64_t word, Error& err) {
  ContextVector* contexts;
  contexts = llscan_->GetContexts();
  contexts->insert(word);
}

void FindJSObjectsVisitor::InsertOnMapsToInstances(
    uint64_t word, v8::Map map, FindJSObjectsVisitor::MapCacheEntry map_info,
    Error& err) {
  TypeRecord* t;

  auto entry = std::make_pair(map_info.type_name, nullptr);
  auto pp = &llscan_->GetMapsToInstances().insert(entry).first->second;
  // No entry in the map, create a new one.
  if (*pp == nullptr) *pp = new TypeRecord(map_info.type_name);
  t = *pp;
  t->AddInstance(word, map.InstanceSize(err));
}

void FindJSObjectsVisitor::InsertOnDetailedMapsToInstances(
    uint64_t word, v8::Map map, FindJSObjectsVisitor::MapCacheEntry map_info,
    Error& err) {
  DetailedTypeRecord* t;

  auto type_name_with_properties = map_info.GetTypeNameWithProperties();

  auto entry = std::make_pair(type_name_with_properties, nullptr);
  auto pp = &llscan_->GetDetailedMapsToInstances().insert(entry).first->second;
  // No entry in the map, create a new one.
  if (*pp == nullptr) {
    auto type_name_with_three_properties = map_info.GetTypeNameWithProperties(
        MapCacheEntry::kDontShowArrayLength,
        kNumberOfPropertiesForDetailedOutput);
    *pp = new DetailedTypeRecord(type_name_with_three_properties,
                                 map_info.own_descriptors_count_,
                                 map_info.indexed_properties_count_);
  }
  t = *pp;
  t->AddInstance(word, map.InstanceSize(err));
}


bool FindJSObjectsVisitor::IsAHistogramType(v8::Map& map, Error& err) {
  int64_t type = map.GetType(err);
  if (err.Fail()) return false;

  v8::LLV8* v8 = map.v8();
  if (v8::JSObject::IsObjectType(v8, type)) return true;
  if (type == v8->types()->kJSArrayType) return true;
  if (type == v8->types()->kJSTypedArrayType) return true;
  if (type < v8->types()->kFirstNonstringType) return true;
  return false;
}


bool LLScan::ScanHeapForObjects(lldb::SBTarget target,
                                lldb::SBCommandReturnObject& result) {
  /* Check the last scan is still valid - the process hasn't moved
   * and we haven't changed target.
   */

  // Reload process anyway
  process_ = target.GetProcess();

  // Need to reload memory regions.
  if (target_ != target) {
    ClearMapsToInstances();
    ClearReferences();
    target_ = target;
  }

  /* If we've reached here we have access to information about the valid memory
   * regions in the process and can scan for objects.
   */

  /* Populate the map of objects. */
  if (mapstoinstances_.empty()) {
    FindJSObjectsVisitor v(target, this);

    ScanMemoryRegions(v);
  }

  return true;
}

std::string FindJSObjectsVisitor::MapCacheEntry::GetTypeNameWithProperties(
    ShowArrayLength show_array_length, size_t max_properties) {
  std::string type_name_with_properties(type_name);

  if (show_array_length == kShowArrayLength) {
    type_name_with_properties +=
        "[" + std::to_string(indexed_properties_count_) + "]";
  }

  size_t i = 0;
  max_properties = max_properties ? std::min(max_properties, properties_.size())
                                  : properties_.size();
  for (auto it = properties_.begin(); i < max_properties; ++it, i++) {
    type_name_with_properties += (i ? ", " : ": ") + *it;
  }
  if (max_properties < properties_.size()) {
    type_name_with_properties += ", ...";
  }
  
  return type_name_with_properties;
}


bool FindJSObjectsVisitor::MapCacheEntry::Load(v8::Map map,
                                               v8::HeapObject heap_object,
                                               v8::LLV8* llv8, Error& err) {
  is_histogram = false;

  is_context = v8::Context::IsContext(llv8, heap_object, err);
  if (err.Fail()) return false;
  if (is_context) return true;

  // Check type first
  is_histogram = FindJSObjectsVisitor::IsAHistogramType(map, err);

  // On success load type name
  if (is_histogram) type_name = heap_object.GetTypeName(err);

  v8::HeapObject descriptors_obj = map.InstanceDescriptors(err);
  RETURN_IF_INVALID(descriptors_obj, false);

  v8::DescriptorArray descriptors(descriptors_obj);
  own_descriptors_count_ = map.NumberOfOwnDescriptors(err);
  if (err.Fail()) return false;

  int64_t type = map.GetType(err);
  indexed_properties_count_ = 0;
  if (v8::JSObject::IsObjectType(llv8, type) ||
      (type == llv8->types()->kJSArrayType)) {
    v8::JSObject js_obj(heap_object);
    indexed_properties_count_ = js_obj.GetArrayLength(err);
    if (err.Fail()) return false;
  }

  for (uint64_t i = 0; i < own_descriptors_count_; i++) {
    v8::Value key = descriptors.GetKey(i);
    if (!key.Check()) continue;
    properties_.emplace_back(key.ToString(err));
  }

  return true;
}


inline static ByteOrder GetHostByteOrder() {
  union {
    uint8_t a[2];
    uint16_t b;
  } u = {{0, 1}};
  return u.b == 1 ? ByteOrder::eByteOrderBig : ByteOrder::eByteOrderLittle;
}

void LLScan::ScanMemoryRegions(FindJSObjectsVisitor& v) {
  const uint64_t addr_size = process_.GetAddressByteSize();
  bool swap_bytes = process_.GetByteOrder() != GetHostByteOrder();

  // Pages are usually around 1mb, so this should more than enough
  const uint64_t block_size = 1024 * 1024 * addr_size;
  unsigned char* block = new unsigned char[block_size];

  lldb::SBMemoryRegionInfoList memory_regions = process_.GetMemoryRegions();
  lldb::SBMemoryRegionInfo region_info;

  for (uint32_t i = 0; i < memory_regions.GetSize(); ++i) {
    memory_regions.GetMemoryRegionAtIndex(i, region_info);

    if (!region_info.IsWritable()) {
      continue;
    }

    uint64_t address = region_info.GetRegionBase();
    uint64_t len = region_info.GetRegionEnd() - region_info.GetRegionBase();

    /* Brute force search - query every address - but allow the visitor code to
     * say how far to move on so we don't read every byte.
     */

    SBError sberr;
    uint64_t address_end = address + len;

    // Load data in blocks to speed up whole process
    for (auto searchAddress = address; searchAddress < address_end;
         searchAddress += block_size) {
      size_t loaded = std::min(address_end - searchAddress, block_size);
      process_.ReadMemory(searchAddress, block, loaded, sberr);
      if (sberr.Fail()) {
        // TODO(indutny): add error information
        break;
      }

      uint32_t increment = 1;
      for (size_t j = 0; j + addr_size <= loaded;) {
        uint64_t value;

        if (addr_size == 4) {
          value = *reinterpret_cast<uint32_t*>(&block[j]);
          if (swap_bytes) {
            value = __builtin_bswap32(value);
          }
        } else if (addr_size == 8) {
          value = *reinterpret_cast<uint64_t*>(&block[j]);
          if (swap_bytes) {
            value = __builtin_bswap64(value);
          }
        } else {
          break;
        }

        increment = v.Visit(j + searchAddress, value);
        if (increment == 0) break;

        j += static_cast<size_t>(increment);
      }

      if (increment == 0) {
        break;
      }
    }
  }

  delete[] block;
}

void LLScan::ClearMapsToInstances() {
  TypeRecord* t;
  for (auto entry : mapstoinstances_) {
    t = entry.second;
    delete t;
  }
  mapstoinstances_.clear();
}

void LLScan::ClearReferences() {
  ReferencesVector* references;

  for (auto entry : references_by_value_) {
    references = entry.second;
    delete references;
  }
  references_by_value_.clear();

  for (auto entry : references_by_property_) {
    references = entry.second;
    delete references;
  }
  references_by_property_.clear();

  for (auto entry : references_by_string_) {
    references = entry.second;
    delete references;
  }
  references_by_string_.clear();
}

bool HeapSnapshotJSONSerializer::DoExecute(SBDebugger d, char** cmd, SBCommandReturnObject& result){
  write_.open("core-dump.heapsnapshot");
  SBTarget target = d.GetSelectedTarget();
  if(!target.IsValid()){
    result.SetError("Invalid process..\n");
    return false;
  }

  // Load V8 constants from postmortem data
  llscan_->v8()->Load(target);

  
  /* Ensure we have a map of objects. */
  if (!llscan_->ScanHeapForObjects(target, result)) {
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  DataEntry(err);
  ImplementSnapshot(err);

  write_.close();
  return true;
}

uint64_t HeapSnapshotJSONSerializer::GetChildrenCount(Error &err, uint64_t word){ //Edge count...
  v8::Value v8_value(llscan_->v8(), word); //Looks in llv8.h
  uint64_t childrenCount = 0;
  v8::HeapObject heap_object(v8_value); //Get heap object..
  if(!heap_object.Check()) return -1;
  v8::HeapObject map_object = heap_object.GetMap(err); //Get map object..
  if(err.Fail() || !map_object.Check()) return -1;

  v8::Map map(map_object); // Accessing meta map inside Map object..

  v8::HeapObject descriptors_obj = map.InstanceDescriptors(err);
  if (err.Fail()) return 0;

  v8::DescriptorArray descriptors(descriptors_obj); //From llv8.cc function DescriptorEntries...
  int64_t no_of_own_descriptors_count = map.NumberOfOwnDescriptors(err);
  if (err.Fail()) return {};

  int64_t in_object_properties = map.InObjectProperties(err);
  if (err.Fail()) return {};

  int64_t instance_size = map.InstanceSize(err);
  if (err.Fail()) return 0;

  v8::JSObject js_obj(heap_object);
  v8::HeapObject extra_properties_obj = js_obj.Properties(err);
  if (err.Fail()) return {};

  v8::FixedArray extra_properties(extra_properties_obj);

  // Iterate over elements
  v8::HeapObject elements_obj = js_obj.Elements(err);
  v8::FixedArray elements(elements_obj);
  uint64_t length = js_obj.GetArrayLength(err);
  
  for (int64_t i = 0; i < length; i++) {
    v8::Value value = elements.Get<v8::Value>(i, err);
    if (err.Fail()) return 0;

    bool is_hole = value.IsHole(err);
    if (err.Fail()) return 0;

    // Skip holes
    if (is_hole) continue;

    v8::Smi smi(value);
    if (smi.Check()) {
      continue;
    };

    v8::HeapObject obj(value);
    if (!obj.Check()) {
      continue;
    }

    int64_t type = obj.GetType(err);
    v8::LLV8* v8 = heap_object.v8();
    if (err.Fail()) return false;

    if (type == v8->types()->kOddballType) {
      continue;
    }

    if (type == v8->types()->kJSFunctionType) {
      continue;
    }
    v8::JSObject js_obj(heap_object);
  
    auto scanner = new FindReferencesCmd::ReferenceScanner(llscan_, value);
    if (!scanner->AreReferencesLoaded()) {
      scanner->ScanRefs(js_obj, err);
    }

    auto references = llscan_->GetReferencesByValue(obj.raw());
    auto pos = std::distance(references->begin(),
        std::find(references->begin(), references->end(), word));

    if (pos >= references->size()) {
      // If the reference couldn't be found in the heap, skip
      continue;
    }

    HeapGraphEdge edge;
    edge.type_ = HeapGraphEdge::Type::kElement;
    edge.set_name_or_index(i);
    edge.set_to_address(obj.raw()); //Adds heap objects address to edges deque...
    edges_.push_back(edge);

    childrenCount++;
  }

  for (int64_t i = 0; i < no_of_own_descriptors_count; i++) {
    v8::Value value;
    v8::Smi details = descriptors.GetDetails(i);
    if (!details.Check()) {
      PRINT_DEBUG("Failed to get details for index %ld", i);
      continue;
    }

    v8::Value key = descriptors.GetKey(i);
    if (!key.Check()) continue;

    if (descriptors.IsConstFieldDetails(details) ||
        descriptors.IsDescriptorDetails(details)) {

      value = descriptors.GetValue(i);
      if (!value.Check()) continue;
      continue;
    }
    // Skip non-fields for now, Object.keys(obj) does
    // not seem to return these (for example the "length"
    // field on an array).
    if (!descriptors.IsFieldDetails(details)) continue;

    if (descriptors.IsDoubleField(details)) continue;

    int64_t index = descriptors.FieldIndex(details) - in_object_properties;

    if (index < 0) {
      value = js_obj.GetInObjectValue<v8::Value>(instance_size, index, err);
    } else {
      value = extra_properties.Get<v8::Value>(index, err);
    }
    // Test if this is SMI or if not heap object...
    // Skip inspecting things that look like Smi's, and also if they aren't objects. 
    v8::Smi smi(value);
    v8::HeapObject obj(value); // Takes value of the property and returns its heap object...
    if (!obj.Check() || smi.Check()) {
      continue;
    }

    //Check objects type...
    int64_t type = obj.GetType(err);
    if(err.Fail()) continue;

    v8::LLV8* v8 = heap_object.v8();
    if(type == v8->types()->kJSFunctionType || type == v8->types()->kOddballType){
      continue;
    }

    
    HeapGraphEdge edge;
    edge.type_ = HeapGraphEdge::Type::kProperty;
    edge.set_name_or_index(GetStringId(err, key.ToString(err)));
    edge.set_to_address(obj.raw()); //Adds heap objects address to edges deque...
    edges_.push_back(edge);

    // if(edge.name_or_index() == 107){//Check for not found edge index..
    //     std::cout << edge.to_address() << std::endl;
    //   }else{
    //     std::cout << "Not Found: " << std::endl;
    //   }

    // if(obj.raw() == 32706854294585){ //Just for edge data validation by quering edge data using node's address...
    //   std::cout << "Edge index: " << edge.name_or_index() << ", " << "Edge name: " << key.ToString(err) << std::endl;
    // }
    childrenCount++;
    // std::cout << "String ID: " << GetStringId(err, key.ToString(err)) << std::endl;



  }
  // if(heap_object.GetTypeName(err).find("<unknown>") != std::string::npos){
  //   std::cout << heap_object.GetTypeName(err) << std::endl;
  //   // continue;
  // } 
  
  
  return childrenCount;
}

void HeapSnapshotJSONSerializer::DataEntry(Error &err){
  uint64_t next_id = 1;
  const int step = 2;
  std::map<uint64_t, HeapGraphNode> visitedNode;

  InitialEntry(err, next_id);
  
  AddGCRootsEntry(err, next_id);

  

  for(auto record_map : llscan_->GetMapsToInstances()){
    for(auto record : record_map.second->GetInstances()){
      if(visitedNode.count(record) != 0){ return; } 
      HeapGraphNode node;
      node.set_address(record);
      node.type_ = GetInstanceType(err, record); //Enum type from struct Node..
      node.set_name(GetStringId(err, record_map.second->GetTypeName())); //Index to the string representing the name of this node
      node.set_id(next_id);
      next_id += step;
      node.set_size(GetNodeSelfSize(err, record));
      node.set_children(GetChildrenCount(err, record));
      if(node.children() == -1) continue; 
      if(node.type_ == HeapGraphNode::Type::kInvalid) continue;
      nodes_.push_back(node);
      visitedNode.insert(std::pair<uint64_t, HeapGraphNode>(record, nodes_.back()));
    }
  }
  for(auto& edge : edges_){
    if (visitedNode.count(edge.to_address()) == 0) { 
      edge.set_to_node_id(0);
      continue;
    }
  
    HeapGraphNode node = visitedNode.at(edge.to_address()); 
    
    if((node.address() != edge.to_address())) { //Check address of both node and its edge and if it doesn't match set its corresponding node id to -1
      edge.set_to_node_id(0);
      continue;
    }
    else{
      // std::cout << "Node id: " << node.id() << ", " << node.address() << ", " << "Edge index: " << edge.name_or_index() << std::endl;
      edge.set_to_node_id(node.id()*6); //Otherwise set edge's corresponding from node id..
    }
  }
}

void HeapSnapshotJSONSerializer::InitialEntry(Error &err, uint64_t next_id){
    HeapGraphNode node;
    node.set_address(0);
    node.type_ = HeapGraphNode::Type::kSynthetic;
    node.set_name(GetStringId(err, ""));
    node.set_id(1);
    next_id += 2;
    node.set_size(0);
    node.set_children(0);
    nodes_.push_back(node);
}

void HeapSnapshotJSONSerializer::AddGCRootsEntry(Error &err, uint64_t next_id){
    HeapGraphNode node;
    node.set_address(0);
    node.type_ = HeapGraphNode::Type::kSynthetic;
    node.set_name(GetStringId(err, "(GC roots)"));
    node.set_id(next_id);
    next_id += 2;
    node.set_size(0);
    node.set_children(0);
    nodes_.push_back(node);
}


HeapGraphNode::Type HeapSnapshotJSONSerializer::GetInstanceType(
    Error &err, uint64_t word) {
  v8::Value v8_value(llscan_->v8(), word); //Accessing v8() from LLScan..
  v8::HeapObject heap_object(v8_value);
  int64_t type = heap_object.GetType(err);
  v8::LLV8* v8 = heap_object.v8();
  if(type == v8->types()->kCodeType){ //Accessing types() from llv8.h Type types..
    return HeapGraphNode::Type::kCode;
  }
  if(type == v8->types()->kJSFunctionType){
    return HeapGraphNode::Type::kClosure;
  }
  if(type == *v8->types()->kJSRegExpType){
    return HeapGraphNode::Type::kRegExp;
  }
  if(type == v8->types()->kJSObjectType){
    return HeapGraphNode::Type::kObject;
  }
  if(type == v8->types()->kHeapNumberType){
    return HeapGraphNode::Type::kHeapNumber;
  }
  if(type < v8->types()->kFirstNonstringType){
    v8::String str(heap_object);

    v8::CheckedType<int64_t> str_repr = str.Representation(err);

    if(*str_repr == v8->string()->kConsStringTag){
      return HeapGraphNode::Type::kConsString;
    }else if(*str_repr == v8->string()->kSlicedStringTag){
      return HeapGraphNode::Type::kSlicedString;
    }else{
      return HeapGraphNode::Type::kString;
    }
  }
  if(type == v8->types()->kJSArrayBufferType || 
     type == v8->types()->kJSTypedArrayType  ||
     type == v8->types()->kFixedArrayType    ||
     type == v8->types()->kJSArrayType){
    return HeapGraphNode::Type::kArray;
  }
      
  return HeapGraphNode::Type::kInvalid;
};

uint64_t HeapSnapshotJSONSerializer::GetStringId(Error &err, std::string name) {
  auto position = std::distance(strings_.begin(), find(strings_.begin(), strings_.end(), name)); // Returns position as id of the object name..
  uint64_t index;
  
  if(position >= strings_.size()) {
    index = strings_.size();
    strings_.push_back(name);
  } else {
    index = position;
  }
  // file << index + 1 << ", " << name << std::endl;
  return index + 1;  
}



void HeapSnapshotJSONSerializer::ImplementSnapshot(Error &err) {
  write_ << "{";
  write_ << "\"snapshot\":{";
  SnapshotSerializer(err);
  write_ << "}," << std::endl;
  write_ << "\"nodes\":[";
  SerializeNodes(err);  
  write_ << "]," << std::endl;
  write_ << "\"edges\":[";
  SerializeEdges(err); 
  write_ << "]" << std::endl;
  write_ << "\"trace_function_infos\":[";
  write_ << "]" << std::endl;
  write_ << "\"trace_tree\":[";
  write_ << "]" << std::endl;
  write_ << "\"samples\":[";
  write_ << "]," << std::endl;
  write_ << "\"strings\":[";
  SerializeStrings(err);
  write_ << "]," << std::endl;

}

void HeapSnapshotJSONSerializer::SnapshotSerializer(Error &err){
  write_ << "\"meta\":";
  // The object describing node serialization layout.
  // We use a set of macros to improve readability.
#define JSON_A(s) "[" s "]"
#define JSON_O(s) "{" s "}"
#define JSON_S(s) "\"" s "\""
  write_ << JSON_O(
    JSON_S("node_fields") ":" JSON_A(
        JSON_S("type") ","
        JSON_S("name") ","
        JSON_S("id") ","
        JSON_S("self_size") ","
        JSON_S("edge_count") ","
        JSON_S("trace_node_id")) ","
    JSON_S("node_types") ":" JSON_A(
        JSON_A(
            JSON_S("hidden") ","
            JSON_S("array") ","
            JSON_S("string") ","
            JSON_S("object") ","
            JSON_S("code") ","
            JSON_S("closure") ","
            JSON_S("regexp") ","
            JSON_S("number") ","
            JSON_S("native") ","
            JSON_S("synthetic") ","
            JSON_S("concatenated string") ","
            JSON_S("sliced string")) ","
        JSON_S("string") ","
        JSON_S("number") ","
        JSON_S("number") ","
        JSON_S("number") ","
        JSON_S("number") ","
        JSON_S("number")) ","
    JSON_S("edge_fields") ":" JSON_A(
        JSON_S("type") ","
        JSON_S("name_or_index") ","
        JSON_S("to_node")) ","
    JSON_S("edge_types") ":" JSON_A(
        JSON_A(
            JSON_S("context") ","
            JSON_S("element") ","
            JSON_S("property") ","
            JSON_S("internal") ","
            JSON_S("hidden") ","
            JSON_S("shortcut") ","
            JSON_S("weak")) ","
        JSON_S("string_or_number") ","
        JSON_S("node")) ","
    JSON_S("trace_function_info_fields") ":" JSON_A(
        JSON_S("function_id") ","
        JSON_S("name") ","
        JSON_S("script_name") ","
        JSON_S("script_id") ","
        JSON_S("line") ","
        JSON_S("column")) ","
    JSON_S("trace_node_fields") ":" JSON_A(
        JSON_S("id") ","
        JSON_S("function_info_index") ","
        JSON_S("count") ","
        JSON_S("size") ","
        JSON_S("children")) ","
    JSON_S("sample_fields") ":" JSON_A(
        JSON_S("timestamp_us") ","
        JSON_S("last_assigned_id")));
#undef JSON_S
#undef JSON_O
#undef JSON_A
  write_ << ",\"node_count\":";
  write_ << nodes_.size();

  write_ << ",\"edge_count\":";
  write_ << edges_.size();
  write_ << ",\"trace_function_count\":";
  uint32_t count = 0;
  write_ << count;
}

void HeapSnapshotJSONSerializer::SerializeNodes(Error &err){
  bool initial_node = true;
  for(auto node : nodes_){
    SerializeNode(err, &node, initial_node);
    if(err.Fail()) return;
    initial_node = false;
  }
}

void HeapSnapshotJSONSerializer::SerializeNode(Error& err, HeapGraphNode* node, bool initial_node){
  if(!initial_node){
    write_ << ',';
  }
  write_ << node->type_ << "," << node->name() << "," << node->id() << "," << node->size() << "," << node->children() << "," << node->trace_node_id() << std::endl;
}

uint64_t HeapSnapshotJSONSerializer::GetNodeSelfSize(
    Error &err, uint64_t word) {
  v8::Value v8_value(llscan_->v8(), word); //Looks in llv8.h

  v8::Smi smi(v8_value);  //Check for SMI..
  if (smi.Check()) return 4;

  v8::HeapObject heap_object(v8_value); //Takes Value as default argument
  if (!heap_object.Check()) return -1;

  v8::HeapObject map_object = heap_object.GetMap(err);
  if (err.Fail() || !map_object.Check()) return -1;

  v8::Map map(map_object); //Takes Heap object as default argument V8_VALUE_DEFAULT_METHODS in llv8.h
  return map.InstanceSize(err);
}

void HeapSnapshotJSONSerializer::SerializeEdges(Error &err){
  bool initial_edge = true;
  for(auto edge : edges_){
    SerializeEdge(err, &edge, initial_edge);
    if(err.Fail()) return;
    initial_edge = false;
  }
}

void HeapSnapshotJSONSerializer::SerializeEdge(Error &err, HeapGraphEdge* edge, bool initial_edge){
  if(!initial_edge){
    write_ << ',';
  }
  write_ << edge->type_ << "," << edge->name_or_index() << "," << edge->to_node_id() << std::endl;
}

void HeapSnapshotJSONSerializer::SerializeStrings(Error &err){
  write_ << "\"<dummy>\"";
  for(auto& string : strings_){
    SerializeString(err, string.c_str());
    if(err.Fail()) return;
  }
}

void HeapSnapshotJSONSerializer::SerializeString(Error &err, std::string string){
  write_ << '\"' << string << '\"' << ',' << std::endl;
}
}  // namespace llnode
