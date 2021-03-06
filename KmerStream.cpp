#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <zlib.h>
#include <getopt.h>
#include <sys/stat.h>
#include <fstream>
#include <stdint.h>
#include <sstream>
#include <bitset>

#ifdef _OPENMP
  #include <omp.h>
#endif

#include "common.h"
#include "kseq.h"
#include "StreamCounter.hpp"
#include "RepHash.hpp"
#include "lsb.hpp"
#include "StreamCounter.hpp"

//
#include "KmerIterator.hpp"
#include "Kmer.hpp"
#define SEQAN_HAS_ZLIB 1

// seqan libraries
#include <seqan/bam_io.h>


KSEQ_INIT(gzFile, gzread) 

using namespace std;

typedef vector<pair<string,string> > read_t;


struct ProgramOptions {
  //size_t k;
	vector<int> klist;
  bool verbose;
	bool online;
  bool bam;
  string output;
  vector<string> files;
  double e;
  vector<size_t> q_cutoff;
  size_t q_base;
  size_t threads;
  int seed;
  size_t chunksize;
  ProgramOptions() : verbose(false), online(false),  bam(false), e(0.01), seed(0), threads(1), chunksize(10000), q_base(33) {}
};

void PrintUsage() {
  cerr << "KmerStream " << VERSION << endl << endl;
  cerr << "Estimates occurrences of k-mers in fastq or fasta files and saves results" << endl << endl;
  cerr << "Usage: KmerStream [options] ... FASTQ files";
  cerr << endl << endl <<
    "-k, --kmer-size=INT      Size of k-mers, either a single value or comma separated list" << endl <<
    "-q, --quality-cutoff=INT Comma separated list, keep k-mers with bases above quality threshold in PHRED (default 0)" << endl <<
    "-o, --output=STRING      Filename for output" << endl <<
    "-e, --error-rate=FLOAT   Error rate guaranteed (default value 0.01)" << endl <<
    "-t, --threads=INT        SNumber of threads to use (default value 1)" << endl <<
    "-s, --seed=INT           Seed value for the randomness (default value 0, use time based randomness)" << endl <<
    "-b, --bam                Input is in BAM format (default false)" << endl <<
    "    --verbose            Print lots of messages during run" << endl <<
		"    --online             Prints out estimates every 100K reads" << endl <<
    "    --q64                set if PHRED+64 scores are used (@...h) default used PHRED+33" << endl << endl

    ;
}

void ParseOptions(int argc, char **argv, ProgramOptions &opt) {
  int verbose_flag = 0;
	int online_flag = 0;
  int bam_flag = 0;
  int q64_flag =0;

  const char* opt_string = "k:o:e:s:bt:q:";
  static struct option long_options[] =
  {
    {"verbose", no_argument, &verbose_flag, 1},
		{"online", no_argument, &online_flag, 1},
    {"q64", no_argument, &q64_flag, 1},
    {"bam", no_argument, &bam_flag, 'b'},
    {"threads", required_argument, 0, 't'},
    {"quality-cutoff", required_argument, 0, 'q'},
    {"kmer-size", required_argument, 0, 'k'},
    {"error-rate", required_argument, 0, 'e'},
    {"seed", required_argument, 0, 's'},
    {"output", required_argument, 0, 'o'},
    {0,0,0,0}
  };

  int option_index = 0; 
  int c;
  string qs;
  size_t prev=0,next=0;

  while (true) {
    c = getopt_long(argc,argv,opt_string, long_options, &option_index);

    if (c == -1) {
      break;
    }

    switch (c) {
    case 0:
      break;
    case 'k':
		{
      //opt.k = atoi(optarg);
			next=0,prev=0;
			string ks(optarg);
			while ((next = ks.find(',',prev)) != string::npos) {
				if (next-prev != 0) {
					opt.klist.push_back(atoi(ks.substr(prev,next-prev).c_str()));
				}
				prev = next+1;
			}
			if (prev < ks.size()) {
				opt.klist.push_back(atoi(ks.substr(prev).c_str()));
			}
      break;
		}
    case 'o':
      opt.output = optarg;
      break;
    case 'e':
      opt.e = atof(optarg);
      break;
    case 's':
      opt.seed = atoi(optarg);
      break;
    case 't':
      opt.threads = atoi(optarg);
      break;
    case 'q':
		{
			prev=0,next=0;
      qs = string(optarg);
      while ((next = qs.find(',',prev)) != string::npos) {
				if (next-prev != 0) {
					opt.q_cutoff.push_back(atoi(qs.substr(prev,next-prev).c_str()));
				}
				prev = next+1;
      }
      if (prev < qs.size()) {
				opt.q_cutoff.push_back(atoi(qs.substr(prev).c_str()));
      }
      break;
		}
    default: break;
    }
  }

  // all other arguments are fast[a/q] files to be read
  for (int i = optind; i < argc; i++) {
    opt.files.push_back(argv[i]);
  }

  
  if (verbose_flag) {
    opt.verbose = true;
  }
	if (online_flag) {
		opt.online = true;
	}
  if (bam_flag) {
    opt.bam = true;
  }
  if (q64_flag) {
    opt.q_base = 64;
  }
  if (opt.q_cutoff.empty()) {
    opt.q_cutoff.push_back(0);
  }
}


bool CheckOptions(ProgramOptions &opt) {
  bool ret = true;


	for (int i = 0; i < opt.klist.size(); i++){
		int k = opt.klist[i];
		if (k <= 0) { 
			cerr << "Error, invalid value for kmer-size: " << k << endl;
			cerr << "Value must be at least 1" << endl;
			ret = false;
		}
	}

  if (opt.files.size() == 0) {
    cerr << "Need to specify files for input" << endl;
    ret = false;
  } else {
    struct stat stFileInfo;
    vector<string>::const_iterator it;
    int intStat;
    for(it = opt.files.begin(); it != opt.files.end(); ++it) {
      intStat = stat(it->c_str(), &stFileInfo);
      if (intStat != 0) {
	cerr << "Error: file not found, " << *it << endl;
	ret = false;
      }
    }
  }

  if (opt.threads <= 0) {
    cerr << "Threads need to be positive" << endl;
    ret = false;
  } else {
    #ifdef _OPENMP
    if (opt.threads > (size_t) omp_get_max_threads()) {
      cerr << "Cant use " << opt.threads << " threads, lowering to " << omp_get_max_threads() << endl;
      opt.threads = omp_get_max_threads();
    }
    #endif
  }

  if (opt.q_cutoff[0] < 0) {
    cerr << "Invalid quality score" << endl;
    ret = false;
  }

  return ret;
}

template <typename SP>
void RunFastqStream(const ProgramOptions &opt) {
  std::ios_base::sync_with_stdio(false);
  gzFile fp = 0;
  kseq_t *seq = 0;
  size_t qsize = opt.q_cutoff.size();
	size_t ksize = opt.klist.size();
	
  vector<SP> sps(qsize*ksize,SP(opt));
  for (size_t i = 0; i < qsize; i++) {
		for (size_t j = 0; j < ksize; j++) {
			sps[i*qsize+j].setQualityCutoff(opt.q_cutoff[i]);
			sps[i*qsize+j].setK(opt.klist[j]);
		}
  }
  // iterate over all reads
  int l;

	size_t nreads = 0;
	
  for (vector<string>::const_iterator it = opt.files.begin(); it != opt.files.end(); ++it) {
    //cout << "reading file " << *it << endl;
    fp = gzopen(it->c_str(), "r");
    seq = kseq_init(fp); 
    while ((l = kseq_read(seq)) > 0) {
			nreads++;
      // seq->seq.s is of length seq->seq.l
      for (size_t i = 0; i < qsize*ksize; i++) {
				sps[i](seq->seq.s, seq->seq.l, seq->qual.s, seq->qual.l);
      }

			if (opt.online && (nreads % 100000) == 0) {
				cout << (nreads/1000) << "K reads  -- " <<
					sps[0].humanReport() << endl;
			}
    }
  }

  kseq_destroy(seq); // STEP 5: destroy seq
  gzclose(fp); // STEP 6: close the file handler

  ofstream of;
  of.open(opt.output.c_str(), ios::out);
  for (size_t i = 0 ; i < qsize; i++) {
		for (size_t j = 0; j < ksize; j++) {
			of << "Q = " << opt.q_cutoff[i] << ", k = " << opt.klist[j] << endl;
			of << sps[i*qsize+j].report();
		}
  }
  of.close();


}


template <typename SP>
void RunThreadedFastqStream(const ProgramOptions &opt) {
  std::ios_base::sync_with_stdio(false);
  gzFile fp;  
  kseq_t *seq;
  size_t threads = opt.threads;
  size_t qsize = opt.q_cutoff.size();

  vector<SP> sps(threads*qsize,SP(opt)); 
  for (size_t i = 0; i < qsize; i++) {
    for (size_t j = 0; j < threads; j++) {
      sps[j*qsize + i].setQualityCutoff(opt.q_cutoff[i]);
    }
  }


  // iterate over all reads

  size_t readCount;
  size_t chunk = opt.chunksize;
  read_t reads(chunk);
#ifdef _OPENMP
  omp_set_num_threads(threads);  
#endif 
  for (vector<string>::const_iterator it = opt.files.begin(); it != opt.files.end(); ++it) {
    int l = 1;
    //cout << "reading file " << *it << endl;
    fp = gzopen(it->c_str(), "r");
    seq = kseq_init(fp);
    
    while (l != 0) { // when l is 0 there are no more reads

      readCount = 0;
      reads.clear();
      while ((l = kseq_read(seq) > 0) && readCount < chunk) {  
	readCount++;
	// seq->seq.s is of length seq->seq.l
	// TODO: parallelize this part
	reads.push_back(make_pair(string(seq->seq.s), string(seq->qual.s)));
	//      sp(seq->seq.s, seq->seq.l, seq->qual.s, seq->qual.l);
      }  
      
      // ok, do this in parallel
#pragma omp parallel 
      {
	size_t threadnum = 0;
#ifdef _OPENMP
	threadnum = omp_get_thread_num();
#endif
	
	
#pragma omp for 
	for (size_t j = 0; j < reads.size(); j++) {
	  for (size_t i = 0; i < qsize; i++) {
	    sps[threadnum*qsize + i](reads[j].first.c_str(), reads[j].first.size(), reads[j].second.c_str(), reads[j].second.size());
	  }
	}
	
      }
    }
    kseq_destroy(seq); 
    gzclose(fp); 
  }
  
  // join with thread 0

  for (size_t i = 1; i < threads; i++) {
    for (size_t j = 0; j < qsize; j++) {
      sps[j].join(sps[i*qsize+j]);
    }
  }

  ofstream of;
  of.open(opt.output.c_str(), ios::out);
  for (size_t i = 0; i < qsize; i++) {
    of << "Q = " << opt.q_cutoff[i] << endl;
    of << sps[i].report();
  }
  of.close();
};



template <typename SP>
void RunThreadedBamStream(const ProgramOptions &opt) {
  // TODO: merge this with seqan libs
  std::ios_base::sync_with_stdio(false);
  SP sp(opt);
  // iterate over all reads
  size_t n = 0,t=0;
  for (vector<string>::const_iterator it = opt.files.begin(); it != opt.files.end(); ++it) {
    // open file
    seqan::BamStream bs(it->c_str());
    //    seq = kseq_init(fp);
    seqan::BamAlignmentRecord rec;
    while (!atEnd(bs)) {
      t++;
      if (seqan::readRecord(rec, bs) == 0) {
	sp(seqan::toCString(rec.seq), seqan::length(rec.seq),
	   seqan::toCString(rec.qual), seqan::length(rec.qual));
	n++;
      }
    }  

  }
  //cout << "Records in bam files " << n << " " << t << endl;

  ofstream of;
  of.open(opt.output.c_str(), ios::out);
  of << sp.report();
  of.close();
};


template <typename SP>
void RunBamStream(const ProgramOptions &opt) {
  // TODO: merge this with seqan libs
  std::ios_base::sync_with_stdio(false);
  size_t qsize = opt.q_cutoff.size();
  vector<SP> sps(qsize, SP(opt));
  for (size_t i = 0; i < qsize; i++) {
    sps[i].setQualityCutoff(opt.q_cutoff[i]);
  }

  // iterate over all reads
  size_t n = 0,t=0;
  for (vector<string>::const_iterator it = opt.files.begin(); it != opt.files.end(); ++it) {
    // open file
    seqan::BamStream bs(it->c_str());
    //    seq = kseq_init(fp);
    seqan::BamAlignmentRecord rec;
    while (!atEnd(bs)) {
      t++;
      if (seqan::readRecord(rec, bs) == 0) {
	if (!hasFlagQCNoPass(rec) && !hasFlagDuplicate(rec)) {
	  for (size_t i = 0; i < qsize; i++) {
	    sps[i](seqan::toCString(rec.seq), seqan::length(rec.seq),
		   seqan::toCString(rec.qual), seqan::length(rec.qual));
	  }
	  n++;
	}
      }
    }  

  }
  //cout << "Records in bam files " << n << " " << t << endl;

  ofstream of;
  of.open(opt.output.c_str(), ios::out);
  for (size_t i = 0; i < qsize; i++) {
    of << "Q = " << opt.q_cutoff[i] << endl;
    of << sps[i].report();
  }
  of.close();
};



class ReadHasher {
public:
  ReadHasher(const ProgramOptions &opt) : k(0), hf(), sc(opt.e, opt.seed) {
    if (opt.seed != 0) {
      hf.seed(opt.seed);
    }
  }

  void operator()(const char* s, size_t l, const char* q, size_t ql) {
    // create hashes for all k-mers
    // operate on hashes

    size_t i=0,j=0;
    bool last_valid = false;
    
    if (l < k) {
      return;
    } 
    
    while (j < l) {
      //cout << "(" << i << ", " << j << ", " << last_valid << ")\t" << string(s).substr(i,j-i) << endl;
      // s[i...j-1] is a valid string, all k-mers in s[..j-1] have been processed
      char c = s[j];
      if (c != 'N' && c != 'n') {
	if (last_valid) { 
	  // s[i..j-1] was a valid k-mer k-mer, update 
	  //cout << "out,in = " << s[i] << ", " << s[j] << endl;
	  hf.update(s[i],s[j]);
	  i++;
	  j++;
	} else {
	  if (i + k -1 == j) {
	    hf.init(s+i); // start the k-mer at position i
	    //cout << " new valid k-mer" << endl;
	    last_valid = true;
	    j++;
	  } else {
	    j++; // move along
	  }
	}
      } else {
	// invalid character, restart
	j++;
	i = j;
	last_valid = false;
      }

      if (last_valid) {
	//cout << "hash value " << hf.hash() << endl;
	handle(hf.hash());
      }
    }
  }

  void handle(uint64_t val) {
    sc(val);
  }

  void setQualityCutoff(size_t q) {}


	void setK(size_t _k) {
		k = _k;
		hf.init(k);
	}

  string report() {
    return sc.report();
  }

  bool join(const ReadHasher& o) {
    return sc.join(o.sc);
  }

private:


  RepHash hf;
  size_t k;
  StreamCounter sc;
  //F2Counter f2;
  //SumCounter sum;
};


class ReadQualityHasher {
public:
  ReadQualityHasher(const ProgramOptions &opt) : k(0), hf(), sc(opt.e, opt.seed), q_cutoff(0), q_base(opt.q_base) {
    if (opt.seed != 0) {
      hf.seed(opt.seed);
    }
  }

  void setQualityCutoff(size_t q) {
    q_cutoff = q;
  }

	void setK(size_t _k) {
		k = _k;
		hf.init(k);
	}

  void operator()(const char* s, size_t l, const char* q, size_t ql) {
    // create hashes for all k-mers
    // operate on hashes

    size_t i=0,j=0;
    bool last_valid = false;
    
    if (l < k) {
      return;
    } 
    
    while (j < l) {
      //cout << "(" << i << ", " << j << ", " << last_valid << ")\t" << string(s).substr(i,j-i) << endl;
      // s[i...j-1] is a valid string, all k-mers in s[..j-1] have been processed
      char c = s[j];
      if (c != 'N' && c != 'n' && (q[j] >= (char) (q_base+q_cutoff))) {
	if (last_valid) { 
	  // s[i..j-1] was a valid k-mer k-mer, update 
	  //cout << "out,in = " << s[i] << ", " << s[j] << endl;
	  hf.update(s[i],s[j]);
	  i++;
	  j++;
	} else {
	  if (i + k -1 == j) {
	    hf.init(s+i); // start the k-mer at position i
	    //cout << " new valid k-mer" << endl;
	    last_valid = true;
	    j++;
	  } else {
	    j++; // move along
	  }
	}
      } else {
	// invalid character, restart
	j++;
	i = j;
	last_valid = false;
      }

      if (last_valid) {
	//cout << "hash value " << hf.hash() << endl;
	handle(hf.hash());
      }
    }
  }

  void handle(uint64_t val) {
    sc(val);
  }

	string humanReport() {
		return sc.humanReport();
	}
	
  string report() {
    return sc.report();
  }

  bool join(const ReadQualityHasher& o) {
    return sc.join(o.sc);
  }

private:

  size_t q_cutoff,q_base;
  RepHash hf;
  size_t k;
  StreamCounter sc;
  //F2Counter f2;
  //SumCounter sum;
};


int main(int argc, char** argv) {
  ProgramOptions opt;
  ParseOptions(argc,argv,opt);
  if (!CheckOptions(opt)) {
    PrintUsage();
    exit(1);
  }
  
  //
  //Kmer::set_k(opt.k);
  //bool use_qual = opt.q_cutoff.empty() || (*(opt.q_cutoff.begin()) != 0);
  if (opt.bam) {
    /*
    if (!use_qual) {
      RunBamStream<ReadHasher>(opt);
    } else {
      RunBamStream<ReadQualityHasher>(opt);
    }
    */
    if (opt.threads > 1) {
      RunThreadedBamStream<ReadQualityHasher>(opt);
    } else {
      RunBamStream<ReadQualityHasher>(opt);
    }
  } else {
    /*if (!use_qual) {
      RunFastqStream<ReadHasher>(opt);
    } else {
      RunFastqStream<ReadQualityHasher>(opt);
    }
    */
    if (opt.threads > 1) {
      RunThreadedFastqStream<ReadQualityHasher>(opt);
    } else {
      //cerr << "running non-threaded version" << endl;
      RunFastqStream<ReadQualityHasher>(opt);
    }
  }
}


