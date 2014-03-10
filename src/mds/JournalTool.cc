// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab


#include <sstream>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>

#include "common/ceph_argparse.h"
#include "common/errno.h"
#include "osdc/Journaler.h"
#include "mds/mdstypes.h"
#include "mds/LogEvent.h"
#include "include/rados/librados.hpp"

// Hack, special case for getting metablob, replace with generic
#include "mds/events/EUpdate.h"

#include "JournalTool.h"

#define dout_subsys ceph_subsys_mds

using librados::Rados;
using librados::IoCtx;


void JournalTool::usage()
{
  std::cout << "Usage: "
    << "  cephfs-journal-tool [options] journal [inspect|import|export]\n"
    << "  cephfs-journal-tool [options] header <get|set <field> <value>\n"
    << "  cephfs-journal-tool [options] event <selector> <effect> <output>\n"
    << "    <selector>:  [--by-type=<metablob|client|mds|...?>|--by-inode=<inode>|--by-path=<path>|by-tree=<path>|by-range=<N>..<M>|by-dirfrag-name=<dirfrag id>,<name>]\n"
    << "    <effect>: [get|splice]\n"
    << "    <output>: [summary|binary|json] [-o <path>] [--latest]\n"
    << "\n"
    << "Options:\n"
    << "  --rank=<int>  Journal rank (default 0)\n";

  generic_client_usage();
}

JournalTool::~JournalTool()
{
}

void JournalTool::init()
{
  MDSUtility::init();
}

void JournalTool::shutdown()
{
  MDSUtility::shutdown();
}


/**
 * Handle arguments and hand off to journal/header/event mode
 */
int JournalTool::main(std::vector<const char*> argv)
{
  dout(10) << "JournalTool::main " << dendl;
  if (argv.size() < 3) {
    usage();
    return -EINVAL;
  }

  std::vector<const char*>::iterator j = argv.begin();
  for (; j != argv.end(); ++j) {
    dout(10) << "argv '" << *j << "'" << dendl;
  }

  std::vector<const char*>::iterator i = argv.begin();
  // Common args
  std::string rank_str;
  if(ceph_argparse_witharg(argv, i, &rank_str, "--rank")) {
    std::string rank_err;
    rank = strict_strtol(rank_str.c_str(), 10, &rank_err);
    if (!rank_err.empty()) {
        derr << "Bad rank '" << rank_str << "'" << dendl;
        usage();
    }
  }

  std::string mode;
  if (i == argv.end()) {
    derr << "Missing mode [journal|header|event]" << dendl;
    return -EINVAL;
  } else {
    mode = std::string(*i);
    ++i;
  }

  dout(4) << "Initializing for rank " << rank << dendl;

  if (mode == std::string("journal")) {
    return main_journal(std::vector<const char*>(i, argv.end()));
  } else if (mode == std::string("event")) {
    return main_event(std::vector<const char*>(i, argv.end()));
  } else {
    derr << "Bad command '" << mode << "'" << dendl;
    usage();
    return -EINVAL;
  }
}


/**
 * Handle arguments for 'journal' mode
 *
 * This is for operations that act on the journal as a whole.
 */
int JournalTool::main_journal(std::vector<const char*> argv)
{
    std::string command = argv[0];
    if (command == "inspect") {
      return journal_inspect();
    } else {
      derr << "Bad journal command '" << command << "'" << dendl;
      return -EINVAL;
    }
}


/**
 * Parse arguments and execute for 'header' mode
 *
 * This is for operations that act on the header only.
 */
int JournalTool::main_header(std::vector<const char*> argv)
{
  return 0;
}


/**
 * Parse arguments and execute for 'event' mode
 *
 * This is for operations that act on LogEvents within the log
 */
int JournalTool::main_event(std::vector<const char*> argv)
{
  int r;

  std::vector<const char*>::iterator arg = argv.begin();

  JournalScanner js(rank, mdsmap->get_metadata_pool());
  std::string command = *(arg++);
  // TODO: parse filter arguments between command verb and output verb
  if (arg == argv.end()) {
    derr << "Missing output command" << dendl;
    usage();
  }
  std::string output_verb = *arg;

  if (command == "get") {
    r = js.scan();
    if (r) {
      derr << "Failed to scan journal (" << cpp_strerror(r) << ")" << dendl;
      return r;
    }
  } else {
    derr << "Bad journal command '" << command << "'" << dendl;
    return -EINVAL;
  }

  if (output_verb == "binary") {
    // Binary output, files
    std::string output_dir = "dump/";
    boost::filesystem::create_directories(boost::filesystem::path(output_dir));
    for (JournalScanner::EventMap::iterator i = js.events.begin(); i != js.events.end(); ++i) {
      LogEvent *le = i->second;
      bufferlist le_bin;
      le->encode(le_bin);

      std::stringstream filename;
      filename << "0x" << std::hex << i->first << std::dec << "_" << le->get_type_str() << ".bin";
      std::string const path = output_dir + filename.str();
      boost::filesystem::ofstream bin_file(path, std::ofstream::out | std::ofstream::binary);
      le_bin.write_stream(bin_file);
      bin_file.close();
    }
  } else if (output_verb == "summary") {
    for (JournalScanner::EventMap::iterator i = js.events.begin(); i != js.events.end(); ++i) {
      std::string path;
      if (i->second->get_type() == EVENT_UPDATE) {
        EUpdate *eu = reinterpret_cast<EUpdate*>(i->second);
        path = eu->metablob.get_path();
      }
      dout(1) << "0x" << std::hex << i->first << std::dec << " " << i->second->get_type_str() << ": " << path << dendl;
    }
  } else {
    derr << "Bad output command '" << output_verb << "'" << dendl;
    return -EINVAL;
  }

  return 0;
}

/**
 * Provide the user with information about the condition of the journal,
 * especially indicating what range of log events is available and where
 * any gaps or corruptions in the journal are.
 */
int JournalTool::journal_inspect()
{
  int r;

  JournalScanner js(rank, mdsmap->get_metadata_pool());
  r = js.scan();
  if (r) {
    derr << "Failed to scan journal (" << cpp_strerror(r) << ")" << dendl;
    return r;
  }

  dout(1) << "Journal scanned, healthy=" << js.is_healthy() << dendl;

  return 0;
}

std::string JournalScanner::obj_name(uint64_t offset) const
{
  char header_name[60];
  snprintf(header_name, sizeof(header_name), "%llx.%08llx",
      (unsigned long long)(MDS_INO_LOG_OFFSET + rank),
      (unsigned long long)offset);
  return std::string(header_name);
}

/**
 * Read journal header, followed by sequential scan through journal space.
 *
 * Return 0 on success, else error code.  Note that success has the special meaning
 * that we were able to apply our checks, it does *not* mean that the journal is
 * healthy.
 */
int JournalScanner::scan()
{
  int r = 0;
  dout(4) << "JournalScanner::scan: connecting to RADOS..." << dendl;

  Rados rados;
  r = rados.init_with_context(g_ceph_context);
  if (r < 0) {
    derr << "RADOS unavailable, cannot scan filesystem journal" << dendl;
    return r;
  }
  rados.connect();

  dout(4) << "JournalScanner::scan: resolving pool " << pool_id << dendl;
  std::string pool_name;
  r = rados.pool_reverse_lookup(pool_id, &pool_name);
  if (r < 0) {
    derr << "Pool " << pool_id << " named in MDS map not found in RADOS!" << dendl;
    return r;
  }

  dout(4) << "JournalScanner::scan: creating IoCtx.." << dendl;
  IoCtx io;
  r = rados.ioctx_create(pool_name.c_str(), io);
  assert(r == 0);

  r = scan_header(io);
  if (r < 0) {
    return r;
  }
  r = scan_events(io);
  if (r < 0) {
    return r;
  }

  return 0;
}

int JournalScanner::scan_header(librados::IoCtx &io)
{
  int r;

  bufferlist header_bl;
  std::string header_name = obj_name(0);
  dout(4) << "JournalScanner::scan: reading header object '" << header_name << "'" << dendl;
  r = io.read(header_name, header_bl, INT_MAX, 0);
  if (r < 0) {
    derr << "Header " << header_name << " is unreadable" << dendl;
    return 0;  // "Successfully" found an error
  } else {
    header_present = true;
  }

  bufferlist::iterator header_bl_i = header_bl.begin();
  header = new Journaler::Header();
  try
  {
    header->decode(header_bl_i);
  }
  catch (buffer::error e)
  {
    derr << "Header is corrupt (" << e.what() << ")" << dendl;
    return 0;  // "Successfully" found an error
  }

  if (header->magic != std::string(CEPH_FS_ONDISK_MAGIC)) {
    derr << "Header is corrupt (bad magic)" << dendl;
    return 0;  // "Successfully" found an error
  }
  if (!((header->trimmed_pos <= header->expire_pos) && (header->expire_pos <= header->write_pos))) {
    derr << "Header is corrupt (inconsistent offsets)" << dendl;
    return 0;  // "Successfully" found an error
  }
  header_valid = true;

  return 0;
}

int JournalScanner::scan_events(librados::IoCtx &io)
{
  int r;

  uint64_t object_size = g_conf->mds_log_segment_size;
  if (object_size == 0) {
    // Default layout object size
    object_size = g_default_file_layout.fl_object_size;
  }

  uint64_t read_offset = header->expire_pos;
  dout(10) << std::hex << "Header 0x"
    << header->trimmed_pos << " 0x"
    << header->expire_pos << " 0x"
    << header->write_pos << std::dec << dendl;
  dout(10) << "Starting journal scan from offset 0x" << std::hex << read_offset << std::dec << dendl;

  // TODO also check for extraneous objects before the trimmed pos or after the write pos,
  // which would indicate a bogus header.

  bufferlist read_buf;
  bool gap = false;
  uint64_t gap_start = -1;
  for (
      uint64_t obj_offset = (read_offset / object_size);
      obj_offset <= (header->write_pos / object_size);
      obj_offset++) {


    // Read this journal segment
    bufferlist this_object;
    r = io.read(obj_name(obj_offset), this_object, INT_MAX, 0);
    this_object.copy(0, this_object.length(), read_buf);

    // Handle absent journal segments
    if (r < 0) {
      derr << "Missing object " << obj_name(obj_offset) << dendl;
      objects_missing.push_back(obj_offset);
      gap = true;
      gap_start = read_offset;
      continue;
    } else {
      objects_valid.push_back(obj_name(obj_offset));
    }

    // Consume available events
    if (gap) {
      // We're coming out the other side of a gap, search up to the next sentinel
      dout(4) << "Searching for sentinel from 0x" << std::hex << read_buf.length() << std::dec << " bytes available" << dendl;
      // TODO
      // 1. Search forward for sentinel
      // 2. When you find sentinel, point read_offset at it and try reading an entry, especially validate that start_ptr is
      //    correct.
      assert(0);
    } else {
      dout(10) << "Parsing data, 0x" << std::hex << read_buf.length() << std::dec << " bytes available" << dendl;
      while(true) {
        uint32_t entry_size = 0;
        uint64_t start_ptr = 0;
        uint64_t entry_sentinel;
        bufferlist::iterator p = read_buf.begin();
        if (read_buf.length() >= sizeof(entry_sentinel) + sizeof(entry_size)) {
          ::decode(entry_sentinel, p);
          ::decode(entry_size, p);
        } else {
          // Out of data, continue to read next object
          break;
        }

        if (entry_sentinel != Journaler::sentinel) {
          dout(4) << "Invalid sentinel at 0x" << std::hex << read_offset << std::dec << dendl;
          gap = true;
          gap_start = read_offset;
          break;
        }

        if (read_buf.length() >= sizeof(entry_sentinel) + sizeof(entry_size) + entry_size + sizeof(start_ptr)) {
          dout(10) << "Attempting decode at 0x" << std::hex << read_offset << std::dec << dendl;
          bufferlist le_bl;
          read_buf.splice(0, sizeof(entry_sentinel));
          read_buf.splice(0, sizeof(entry_size));
          read_buf.splice(0, entry_size, &le_bl);
          read_buf.splice(0, sizeof(start_ptr));
          // TODO: read out start_ptr and ensure that it points back to read_offset
          LogEvent *le = LogEvent::decode(le_bl);
          if (le) {
            dout(10) << "Valid entry at 0x" << std::hex << read_offset << std::dec << dendl;
            if (true) { // TODO implement filtering
              events[read_offset] = le;
            } else {
              delete le;
            }
            events_valid.push_back(read_offset);
            read_offset += sizeof(entry_sentinel) + sizeof(entry_size) + entry_size + sizeof(start_ptr);
          } else {
            dout(10) << "Invalid entry at 0x" << std::hex << read_offset << std::dec << dendl;
            // Ensure we don't hit this entry again when scanning for next sentinel
            read_offset += 1;
            gap = true;
            gap_start = read_offset;
          }
        } else {
          // Out of data, continue to read next object
          break;
        }
      }
    }
  }

  if (gap) {
    // Ended on a gap, assume it ran to end
    ranges_invalid.push_back(Range(gap_start, -1));
  }

  dout(4) << "Scanned objects, " << objects_missing.size() << " missing, " << objects_valid.size() << " valid" << dendl;
  dout(4) << "Events scanned, " << ranges_invalid.size() << " gaps" << dendl;
  dout(4) << "Found " << events_valid.size() << " valid events" << dendl;

  return 0;
}

JournalScanner::~JournalScanner()
{
  if (header) {
    delete header;
    header = NULL;
  }
  for (EventMap::iterator i = events.begin(); i != events.end(); ++i) {
    delete i->second;
  }
  events.clear();
}

bool JournalScanner::is_healthy() const
{
  return (header_present && header_valid && ranges_invalid.empty() && objects_missing.empty());
}
