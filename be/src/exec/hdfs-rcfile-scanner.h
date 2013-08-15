// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_EXEC_HDFS_RCFILE_SCANNER_H
#define IMPALA_EXEC_HDFS_RCFILE_SCANNER_H

// org.apache.hadoop.hive.ql.io.RCFile is the original RCFile implementation
// and should be viewed as the canonical definition of this format. If
// anything is unclear in this file you should consult the code in
// org.apache.hadoop.hive.ql.io.RCFile.
//
// The following is a pseudo-BNF grammar for RCFile. Comments are prefixed
// with dashes:
//
// rcfile ::=
//   <file-header>
//   <rcfile-rowgroup>+
//
// file-header ::=
//   <file-version-header>
//   <file-key-class-name>              (only exists if version is seq6)
//   <file-value-class-name>            (only exists if version is seq6)
//   <file-is-compressed>
//   <file-is-block-compressed>         (only exists if version is seq6)
//   [<file-compression-codec-class>]
//   <file-header-metadata>
//   <file-sync-field>
//
// -- The normative RCFile implementation included with Hive is actually
// -- based on a modified version of Hadoop's SequenceFile code. Some
// -- things which should have been modified were not, including the code
// -- that writes out the file version header. Consequently, RCFile and
// -- SequenceFile originally shared the same version header.  A newer
// -- release has created a unique version string.
//
// file-version-header ::= Byte[4] {'S', 'E', 'Q', 6}
//                     |   Byte[4] {'R', 'C', 'F', 1}
//
// -- The name of the Java class responsible for reading the key buffer
// -- component of the rowgroup.
//
// file-key-class-name ::=
//   Text {"org.apache.hadoop.hive.ql.io.RCFile$KeyBuffer"}
//
// -- The name of the Java class responsible for reading the value buffer
// -- component of the rowgroup.
//
// file-value-class-name ::=
//   Text {"org.apache.hadoop.hive.ql.io.RCFile$ValueBuffer"}
//
// -- Boolean variable indicating whether or not the file uses compression
// -- for the key and column buffer sections.
//
// file-is-compressed ::= Byte[1]
//
// -- A boolean field indicating whether or not the file is block compressed.
// -- This field is *always* false. According to comments in the original
// -- RCFile implementation this field was retained for backwards
// -- compatability with the SequenceFile format.
//
// file-is-block-compressed ::= Byte[1] {false}
//
// -- The Java class name of the compression codec iff <file-is-compressed>
// -- is true. The named class must implement
// -- org.apache.hadoop.io.compress.CompressionCodec.
// -- The expected value is org.apache.hadoop.io.compress.GzipCodec.
//
// file-compression-codec-class ::= Text
//
// -- A collection of key-value pairs defining metadata values for the
// -- file. The Map is serialized using standard JDK serialization, i.e.
// -- an Int corresponding to the number of key-value pairs, followed by
// -- Text key and value pairs. The following metadata properties are
// -- mandatory for all RCFiles:
// --
// -- hive.io.rcfile.column.number: the number of columns in the RCFile
//
// file-header-metadata ::= Map<Text, Text>
//
// -- A 16 byte marker that is generated by the writer. This marker appears
// -- at regular intervals at the beginning of rowgroup-headers, and is
// -- intended to enable readers to skip over corrupted rowgroups.
//
// file-sync-hash ::= Byte[16]
//
// -- Each row group is split into three sections: a header, a set of
// -- key buffers, and a set of column buffers. The header section includes
// -- an optional sync hash, information about the size of the row group, and
// -- the total number of rows in the row group. Each key buffer
// -- consists of run-length encoding data which is used to decode
// -- the length and offsets of individual fields in the corresponding column
// -- buffer.
//
// rcfile-rowgroup ::=
//   <rowgroup-header>
//   <rowgroup-key-data>
//   <rowgroup-column-buffers>
//
// rowgroup-header ::=
//   [<rowgroup-sync-marker>, <rowgroup-sync-hash>]
//   <rowgroup-record-length>
//   <rowgroup-key-length>
//   <rowgroup-compressed-key-length>
//
// -- rowgroup-key-data is compressed if the column data is compressed.
// rowgroup-key-data ::=
//   <rowgroup-num-rows>
//   <rowgroup-key-buffers>
//
// -- An integer (always -1) signaling the beginning of a sync-hash
// -- field.
//
// rowgroup-sync-marker ::= Int
//
// -- A 16 byte sync field. This must match the <file-sync-hash> value read
// -- in the file header.
//
// rowgroup-sync-hash ::= Byte[16]
//
// -- The record-length is the sum of the number of bytes used to store
// -- the key and column parts, i.e. it is the total length of the current
// -- rowgroup.
//
// rowgroup-record-length ::= Int
//
// -- Total length in bytes of the rowgroup's key sections.
//
// rowgroup-key-length ::= Int
//
// -- Total compressed length in bytes of the rowgroup's key sections.  
//
// rowgroup-compressed-key-length ::= Int
//
// -- Number of rows in the current rowgroup.
//
// rowgroup-num-rows ::= VInt
//
// -- One or more column key buffers corresponding to each column
// -- in the RCFile.
//
// rowgroup-key-buffers ::= <rowgroup-key-buffer>+
//
// -- Data in each column buffer is stored using a run-length
// -- encoding scheme that is intended to reduce the cost of
// -- repeated column field values. This mechanism is described
// -- in more detail in the following entries.
//
// rowgroup-key-buffer ::=
//   <column-buffer-length>
//   <column-buffer-uncompressed-length>
//   <column-key-buffer-length>
//   <column-key-buffer>
//
// -- The serialized length on disk of the corresponding column buffer.
//
// column-buffer-length ::= VInt
//
// -- The uncompressed length of the corresponding column buffer. This
// -- is equivalent to column-buffer-length if the RCFile is not compressed.
//
// column-buffer-uncompressed-length ::= VInt
//
// -- The length in bytes of the current column key buffer
//
// column-key-buffer-length ::= VInt
//
// -- The column-key-buffer contains a sequence of serialized VInt values
// -- corresponding to the byte lengths of the serialized column fields
// -- in the corresponding rowgroup-column-buffer. For example, consider
// -- an integer column that contains the consecutive values 1, 2, 3, 44.
// -- The RCFile format stores these values as strings in the column buffer,
// -- e.g. "12344". The length of each column field is recorded in
// -- the column-key-buffer as a sequence of VInts: 1,1,1,2. However,
// -- if the same length occurs repeatedly, then we replace repeated
// -- run lengths with the complement (i.e. negative) of the number of
// -- repetitions, so 1,1,1,2 becomes 1,~2,2.
//
// column-key-buffer ::= Byte[column-key-buffer-length]
//
// rowgroup-column-buffers ::= <rowgroup-value-buffer>+
//
// -- RCFile stores all column data as strings regardless of the
// -- underlying column type. The strings are neither length-prefixed or
// -- null-terminated, and decoding them into individual fields requires
// -- the use of the run-length information contained in the corresponding
// -- column-key-buffer.
//
// rowgroup-column-buffer ::= Byte[column-buffer-length]
//
// Byte ::= An eight-bit byte
//
// VInt ::= Variable length integer. The high-order bit of each byte
// indicates whether more bytes remain to be read. The low-order seven
// bits are appended as increasingly more significant bits in the
// resulting integer value.
//
// Int ::= A four-byte integer in big-endian format.
//
// Text ::= VInt, Chars (Length prefixed UTF-8 characters)
//
// The above file format is read in chunks.  The "key" buffer is read. The "keys"
// are really the lengths of the column data blocks and the lengths of the values
// within those blocks.  Using this information the column "buffers" (data)
// that are needed by the query are read into a single buffer.  Column data that
// is not used by the query is skipped and not read from the file.  The key data
// and the column data may be compressed.  The key data is compressed in a single
// block while the column data is compressed separately by column.

#include "exec/base-sequence-scanner.h"

namespace impala {

struct HdfsFileDesc;
class HdfsScanNode;
class TupleDescriptor;
class Tuple;

// A scanner for reading RCFiles into tuples. 
class HdfsRCFileScanner : public BaseSequenceScanner {
 public:
  HdfsRCFileScanner(HdfsScanNode* scan_node, RuntimeState* state);
  virtual ~HdfsRCFileScanner();
  
  virtual Status Prepare(ScannerContext* context);

  void DebugString(int indentation_level, std::stringstream* out) const;

 private:
  // The key class name located in the RCFile Header.
  // This is always "org.apache.hadoop.hive.ql.io.RCFile$KeyBuffer"
  static const char* const RCFILE_KEY_CLASS_NAME;

  // The value class name located in the RCFile Header.
  // This is always "org.apache.hadoop.hive.ql.io.RCFile$ValueBuffer"
  static const char* const RCFILE_VALUE_CLASS_NAME;

  // RCFile metadata key for determining the number of columns
  // present in the RCFile: "hive.io.rcfile.column.number"
  static const char* const RCFILE_METADATA_KEY_NUM_COLS;

  // The four byte RCFile unique version header present at the beginning
  // of the file {'R', 'C', 'F' 1} 
  static const uint8_t RCFILE_VERSION_HEADER[4];

  // Implementation of superclass functions.
  virtual FileHeader* AllocateFileHeader();
  virtual Status ReadFileHeader();
  virtual Status InitNewRange();
  virtual Status ProcessRange();

  virtual THdfsFileFormat::type file_format() const { 
    return THdfsFileFormat::RC_FILE; 
  }

  // Reads the RCFile Header Metadata section in the current file to determine the number
  // of columns.  Other pieces of the metadata are ignored.
  Status ReadNumColumnsMetadata();

  // Reads the rowgroup header starting after the sync.
  // Sets:
  //   key_length_
  //   compressed_key_length_
  //   num_rows_
  Status ReadRowGroupHeader();

  // Read the rowgroup key buffers, decompress if necessary.
  // The "keys" are really the lengths for the column values.  They
  // are read here and then used to decode the values in the column buffer.
  // Calls GetCurrentKeyBuffer for each column to process the key data.
  Status ReadKeyBuffers();

  // Process the current key buffer.
  // Inputs:
  //   col_idx: column to process
  //   skip_col_data: if true, just skip over the key data.
  // Input/Output:
  //   key_buf_ptr: Pointer to the buffered file data, this will be moved
  //                past the data for this column.
  // Sets:
  //   col_buf_len_
  //   col_buf_uncompressed_len_
  //   col_key_bufs_
  //   col_bufs_off_
  void GetCurrentKeyBuffer(int col_idx, bool skip_col_data, uint8_t** key_buf_ptr);

  // Read the rowgroup column buffers
  // Sets:
  //   column_buffer_: Fills the buffer with either file data or decompressed data.
  Status ReadColumnBuffers();

  // Look at the next field in the specified column buffer
  // Input:
  //   col_idx: Column of the field.
  // Modifies:
  //   cur_field_length_rep_[col_idx]
  //   key_buf_pos_[col_idx]
  //   cur_field_length_rep_[col_idx]
  //   cur_field_length_[col_idx]
  Status NextField(int col_idx);

  // Read a row group (except for the sync marker and sync) into buffers.
  // Calls:
  //   ReadRowGroupHeader
  //   ReadKeyBuffers
  //   ReadColumnBuffers
  Status ReadRowGroup();
  
  // Reset state for a new row group
  void ResetRowGroup();

  // Move to next row. Calls NextField on each column that we are reading.
  // Modifies:
  //   row_pos_
  Status NextRow();

  enum Version {
    SEQ6,     // Version for sequence file and pre hive-0.9 rc files
    RCF1      // The version post hive-0.9 which uses a new header
  };

  // Data that is fixed across headers.  This struct is shared between scan ranges.
  struct RcFileHeader : public BaseSequenceScanner::FileHeader {
    // RC file version
    Version version;

    // The number of columns in the file (may be more than the number of columns in the
    // table metadata)
    int num_cols;
  };

  // Struct encapsulating all the state for parsing a single column from a row
  // group
  struct ColumnInfo {
    // If true, this column should be materialized, otherwise, it can be skipped
    bool materialize_column;

    // Uncompressed and compressed byte lengths for this column
    int32_t buffer_len;
    int32_t uncompressed_buffer_len;

    // Length and start of the key for this column.
    int32_t key_buffer_len;
    // This is a ptr into the scanner's key_buffer_ for this column.
    uint8_t* key_buffer;
    
    // Current position in the key buffer
    int32_t key_buffer_pos;
  
    // Offset into row_group_buffer_ for the start of this column.
    int32_t start_offset;

    // Offset from the start of the column for the next field in the column
    int32_t buffer_pos;

    // RLE: Length of the current field
    int32_t current_field_len;
    // RLE: Repetition count of the current field
    int32_t current_field_len_rep;
  };
  
  // Vector of column descriptions for each column in the file (i.e., may contain a
  // different number of non-partition columns than are in the table metadata).  Indexed
  // by column index, including non-materialized columns.
  std::vector<ColumnInfo> columns_;

  // Buffer for copying key buffers.  This buffer is reused between row groups.
  std::vector<uint8_t> key_buffer_;
  
  // number of rows in this rowgroup object
  int num_rows_;

  // Current row position in this rowgroup.
  // This value is incremented each time NextRow() is called.
  int row_pos_;

  // Size of the row group's key buffers.
  // Read from the row group header.
  int key_length_;

  // Compressed size of the row group's key buffers.
  // Read from the row group header.
  int compressed_key_length_;

  // Buffer containing the entire row group.  We allocate a buffer for the entire
  // row group, skipping non-materialized columns.
  uint8_t* row_group_buffer_;

  // Sum of the bytes lengths of the materialized columns in the current row group.  This
  // is the number of valid bytes in row_group_buffer_.
  int row_group_length_;

  // This is the allocated size of 'row_group_buffer_'.  'row_group_buffer_' is reused
  // across row groups and will grow as necessary.
  int row_group_buffer_size_;
};

}

#endif
