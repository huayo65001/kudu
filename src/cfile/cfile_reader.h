// Copyright (c) 2012, Cloudera, inc.

#ifndef KUDU_CFILE_CFILE_READER_H
#define KUDU_CFILE_CFILE_READER_H

#include <boost/noncopyable.hpp>
#include <boost/shared_array.hpp>
#include <boost/scoped_ptr.hpp>
#include <tr1/memory>
#include <string>

#include "util/status.h"

namespace kudu {

class RandomAccessFile;

namespace cfile {

class CFileHeaderPB;
class CFileFooterPB;
template <class Key> class IndexTreeIterator;

using std::string;
using boost::shared_array;
using boost::scoped_ptr;
using std::tr1::shared_ptr;


struct ReaderOptions {
};

class CFileIterator;
class BlockPointer;
class IntBlockDecoder;


// Wrapper for a block of data read from a CFile.
// This reference-counts the underlying data, so it can
// be freely copied, and will not be collected until all copies
// have been destructed.
class BlockData {
public:
  BlockData() {}

  BlockData(const Slice &data,
            shared_array<char> data_for_free) :
    data_(data),
    data_for_free_(data_for_free) {
  }

  BlockData(const BlockData &other) :
    data_(other.data_),
    data_for_free_(other.data_for_free_) {
  }

  const Slice &slice() const {
    return data_;
  }

private:
  Slice data_;
  shared_array<char> data_for_free_;
};


class CFileReader : boost::noncopyable {
public:
  CFileReader(const ReaderOptions &options,
              const shared_ptr<RandomAccessFile> &file,
              uint64_t file_size) :
    options_(options),
    file_(file),
    file_size_(file_size),
    state_(kUninitialized) {
  }

  Status Init();

  Status NewIteratorByPos(CFileIterator **iter) const;

  Status ReadBlock(const BlockPointer &ptr,
                   BlockData *ret) const;

  Status SearchPosition(uint32_t pos,
                        BlockPointer *ptr,
                        uint32_t *ret_key);

private:
  Status ReadMagicAndLength(uint64_t offset, uint32_t *len);
  Status ReadAndParseHeader();
  Status ReadAndParseFooter();

  Status GetIndexRootBlock(const string &identifier, BlockPointer *ptr) const;

  const ReaderOptions options_;
  const shared_ptr<RandomAccessFile> file_;
  const uint64_t file_size_;

  enum State {
    kUninitialized,
    kInitialized
  };
  State state_;

  scoped_ptr<CFileHeaderPB> header_;
  scoped_ptr<CFileFooterPB> footer_;
};


class CFileIterator : boost::noncopyable {
public:
  CFileIterator(const CFileReader *reader,
                const BlockPointer &posidx_root);

  Status SeekToOrdinal(uint32_t ord_idx);
  uint32_t GetCurrentOrdinal() const;
  Status GetNextValues(int n, std::vector<uint32_t> *vec);

private:
  // Read the data block currently pointed to by idx_iter_
  // into the dblk_data_ and dblk_ fields.
  //
  // If this returns an error, then the fields
  // have undefined values.
  Status ReadCurrentDataBlock();

  const CFileReader *reader_;

  scoped_ptr<IndexTreeIterator<uint32_t> > idx_iter_;

  bool seeked_;

  BlockData dblk_data_;
  scoped_ptr<IntBlockDecoder> dblk_;
};


} // namespace cfile
} // namespace kudu

#endif
