#include <boost/program_options.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include "picpac.h"
#include "picpac-cv.h"
#include "picpac-util.h"
#include "tar.h"

using namespace std;
using namespace boost;
using namespace picpac;

enum {
    FORMAT_DIR = 0,
    FORMAT_LIST = 1,
    FORMAT_SUB_DIR = 2,
    FORMAT_ANNO_JSON = 3,
    FORMAT_ANNO_IMAGE = 4,
    FORMAT_PICPAC = 5,
    FORMAT_IMAGENET_TARS = 6
};

class Paths: public vector<fs::path> {
public:
    Paths () {}
    Paths (fs::path const &path) {
        fs::recursive_directory_iterator it(fs::path(path), fs::symlink_option::recurse), end;
        for (; it != end; ++it) {
            if (it->status().type() == fs::regular_file) {
                push_back(it->path());
            }
        }
        if (size() < 10) {
            LOG(WARNING) << "Need at least 10 files to train: " << path;
        }
    }
};

class Samples: public vector<Paths> {
public:
    Samples () {}
    Samples (fs::path const &root) {
        fs::directory_iterator it(root), end;
        vector<unsigned> cats;
        for (; it != end; ++it) {
            if (!fs::is_directory(it->path())) {
                LOG(ERROR) << "Not a directory: " << it->path();
                continue;
            }
            fs::path name = it->path().filename();
            try {
                unsigned c = lexical_cast<unsigned>(name.native());
                cats.push_back(c);
            }
            catch (...) {
                LOG(ERROR) << "Category directory not properly named: " << it->path();
            }
        }
        sort(cats.begin(), cats.end());
        cats.resize(unique(cats.begin(), cats.end()) - cats.begin());
        CHECK(cats.size() >= 2) << "Need at least 2 categories to train.";
        CHECK((cats.front() == 0)
                && (cats.back() == cats.size() -1 ))
            << "Subdirectories must be consecutively named from 0 to N-1.";
        for (unsigned c = 0; c < cats.size(); ++c) {
            emplace_back(root / fs::path(lexical_cast<string>(c)));
            LOG(INFO) << "Loaded " << back().size() << " paths for category " << c << ".";
        }
    }
};


int main(int argc, char **argv) {
    namespace po = boost::program_options; 
    fs::path cache;
    fs::path input_path;
    fs::path output_path;
    string encode;
    int max_size;
    int resize;
    int format;
    int limit;
    int quality = 0;

    po::options_description desc("Allowed options");
    desc.add_options()
    ("help,h", "produce help message.")
    ("input,i", po::value(&input_path), "")
    ("output,o", po::value(&output_path), "")
    ("max", po::value(&max_size)->default_value(-1), "")
    ("resize", po::value(&resize)->default_value(-1), "")
    ("format,f", po::value(&format)->default_value(1), "")
    ("cache", po::value(&cache)->default_value(".picpac_cache"), "")
    ("compact", "")
    ("limit", po::value(&limit)->default_value(0), "")
    ("encode", po::value(&encode), "")
    ("jpeg_quality", po::value(&quality), "")
    /*
    ("gray", "")
    ("log-level,v", po::value(&FLAGS_minloglevel)->default_value(1), "")
    */
    ;

    po::positional_options_description p;
    p.add("input", 1);
    p.add("output", 1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
                     options(desc).positional(p).run(), vm);
    po::notify(vm); 

    if (vm.count("help") || input_path.empty() || output_path.empty()) {
        cerr << "Usage:  picpac-import [options] <input> <output>" << endl;
        cerr << desc;
        cerr << endl;
        cerr << "Formats:" << endl
             << "  1: list of <image\tlabel>" << endl
             << "  2: scan a directory" << endl
             << "  3: list of <image\tjson-annotation>" << endl
             << "  4: list of <image\tannotation-image>" << endl;

        return 1;
    }
    //if (vm.count("gray")) gray = true;
    int flags = 0;
    if (vm.count("compact")) flags |= picpac::FileWriter::COMPACT;


    google::InitGoogleLogging(argv[0]);
    picpac::FileWriter db(output_path, flags);
    CachedDownloader downloader(cache);
    ImageReader imreader(max_size, resize, cv::IMREAD_UNCHANGED, encode);

    if (vm.count("jpeg_quality")) {
        imreader.params().push_back(CV_IMWRITE_JPEG_QUALITY);
        imreader.params().push_back(quality);
    }   

    int count = 0;

    if (format == FORMAT_DIR || format == FORMAT_SUB_DIR) {
        Samples all;
        if (format == FORMAT_SUB_DIR) {
            Samples tmp(input_path);
            all.swap(tmp);
        }
        else if (format == FORMAT_DIR) {
            all.emplace_back(input_path);
        }
        for (unsigned i = 0; i < all.size(); ++i) {
            for (auto const &path: all[i]) {
                string data;
                imreader.read(path, &data);
                if (data.empty()) {
                    LOG(ERROR) << "not a image: " << path;
                    continue;
                }
                picpac::Record rec(i, data);
                db.append(rec);
                ++count;
            }
        }
    }
    else if (format == FORMAT_IMAGENET_TARS) {
        fs::ifstream is(input_path.c_str());
        string line;
        int l = 0;
        uint32_t id = 0;


        while (getline(is, line)) {
            Tar tar(line);
            string buffer;
            Tar::posix_header const *header;
            int n = 0;
            while (tar.next(&buffer, &header)) {
                string data;
                try {
                    imreader.transcode(buffer, &data);
                    Record rec(l, data);
                    rec.meta().id = id;
                    db.append(rec);
                }
                catch (BadFile const &) {
                    LOG(ERROR) << "bad file in tar " << l << "/" << n << "/" << id;
                }
                ++n;
                ++id;
            }
            LOG(INFO) << line;
            l++;
        }
    }
    else if (format == FORMAT_PICPAC) {
        IndexedFileReader indb(input_path);
        for (unsigned i = 0; i < indb.size(); ++i) {
            if (limit > 0 && i >= limit) break;
            Record in;
            indb.read(i, &in);
            string binary;
            string in_image = in.field_string(0);
            if (in_image.size() > 0) {
                imreader.transcode(in_image, &binary);
            }
            if (in.size() == 1) {
                Record rec(in.meta().label, binary);
                rec.meta().label2 = in.meta().label2;
                db.append(rec);
            }
            else if (in.size() == 2) {
                Record rec(in.meta().label, binary, in.field_string(1));
                rec.meta().label2 = in.meta().label2;
                db.append(rec);
            }
        }
    }
    else {
        fs::ifstream is(input_path.c_str());
        string line;
        while (getline(is, line)) {
            vector<string> ss;
            split(ss, line, is_any_of("\t"), token_compress_off);
            if (ss.size() != 2) {
                cerr << "Bad line: " << line << endl;
                continue;
            }
            try {
                fs::path path = downloader.download(ss[0]);
                string data;
                imreader.read(path, &data);
                if (data.empty()) {
                    LOG(ERROR) << "not a image: " << path;
                    continue;
                }
                if (format == FORMAT_LIST) {
                    float l = lexical_cast<float>(ss[1]);
                    Record record(l, data);
                    db.append(record);
                }
                else if (format == FORMAT_ANNO_JSON) {
                    Record record(0, data, ss[1]);
                    db.append(record);
                }
                else if (format == FORMAT_ANNO_IMAGE) {
                    string data2;
                    if (ss[1].size()) {
                        fs::path path2 = downloader.download(ss[1]);
                        imreader.read(path2, &data2);
                    }
                    Record record(0, data, data2);
                    db.append(record);
                }
                ++count;
            }
            catch (...) {
                LOG(ERROR) << "Fail to load " << ss[0];
            }
        }
    }
    LOG(INFO) << "Loaded " << count << " samples.";

    return 0;
}


