/* MIT License
 *
 * Copyright (c) 2018 Sam Kovaka <skovaka@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pdqsort.h"
#include "mapper.hpp"


u8 Mapper::PathBuffer::MAX_PATH_LEN = 0, 
   Mapper::PathBuffer::TYPE_MASK = 0;

u32 Mapper::PathBuffer::TYPE_ADDS[EventType::NUM_TYPES];

Mapper::PathBuffer::PathBuffer()
    : length_(0),
      prob_sums_(new float[MAX_PATH_LEN+1]) {
}

Mapper::PathBuffer::PathBuffer(const PathBuffer &p) {
    std::memcpy(this, &p, sizeof(PathBuffer));
}

void Mapper::PathBuffer::free_buffers() {
    delete[] prob_sums_;
}

void Mapper::PathBuffer::make_source(Range &range, u16 kmer, float prob) {
    length_ = 1;
    consec_stays_ = 0;
    event_types_ = 0;
    seed_prob_ = prob;
    fm_range_ = range;
    kmer_ = kmer;
    sa_checked_ = false;

    path_type_counts_[EventType::MATCH] = 1;

    //TODO: no loops!
    for (u8 t = 1; t < EventType::NUM_TYPES; t++) {
        path_type_counts_[t] = 0;
    }

    //TODO: don't write this here to speed up source loop
    prob_sums_[0] = 0;
    prob_sums_[1] = prob;
}


void Mapper::PathBuffer::make_child(PathBuffer &p, 
                                    Range &range,
                                    u16 kmer, 
                                    float prob, 
                                    EventType type) {

    length_ = p.length_ + (p.length_ <= MAX_PATH_LEN);
    fm_range_ = range;
    kmer_ = kmer;
    sa_checked_ = p.sa_checked_;
    event_types_ = TYPE_ADDS[type] | (p.event_types_ >> TYPE_BITS);
    consec_stays_ = (p.consec_stays_ + (type == EventType::STAY)) * (type == EventType::STAY);

    std::memcpy(path_type_counts_, p.path_type_counts_, EventType::NUM_TYPES * sizeof(u8));
    path_type_counts_[type]++;


    if (length_ > MAX_PATH_LEN) {
        std::memcpy(prob_sums_, &(p.prob_sums_[1]), MAX_PATH_LEN * sizeof(float));
        prob_sums_[MAX_PATH_LEN] = prob_sums_[MAX_PATH_LEN-1] + prob;
        seed_prob_ = (prob_sums_[MAX_PATH_LEN] - prob_sums_[0]) / MAX_PATH_LEN;
        path_type_counts_[p.type_tail()]--;
    } else {
        std::memcpy(prob_sums_, p.prob_sums_, length_ * sizeof(float));
        prob_sums_[length_] = prob_sums_[length_-1] + prob;
        seed_prob_ = prob_sums_[length_] / length_;
    }


}

void Mapper::PathBuffer::invalidate() {
    length_ = 0;
}

bool Mapper::PathBuffer::is_valid() const {
    return length_ > 0;
}

u8 Mapper::PathBuffer::match_len() const {
    return path_type_counts_[EventType::MATCH];
}

u8 Mapper::PathBuffer::type_head() const {
    return (event_types_ >> (TYPE_BITS*(MAX_PATH_LEN-2))) & TYPE_MASK;
}

u8 Mapper::PathBuffer::type_tail() const {
    return event_types_ & TYPE_MASK;
}

bool Mapper::PathBuffer::is_seed_valid(const UncalledOpts &p,
                                        bool path_ended) const{
    return (fm_range_.length() == 1 || 
                (path_ended &&
                 fm_range_.length() <= p.max_rep_copy_ &&
                 match_len() >= p.min_rep_len_)) &&

           length_ >= p.seed_len_ &&
           (path_ended || type_head() == EventType::MATCH) &&
           (path_ended || path_type_counts_[EventType::STAY] <= p.max_stay_frac_ * p.seed_len_) &&
          seed_prob_ >= p.min_seed_prob_;
}


bool operator< (const Mapper::PathBuffer &p1, 
                const Mapper::PathBuffer &p2) {
    return p1.fm_range_ < p2.fm_range_ ||
           (p1.fm_range_ == p2.fm_range_ && 
            p1.seed_prob_ < p2.seed_prob_);
}

Mapper::Mapper(const UncalledOpts &ap)
    : 
      opts_(ap),
      model_(ap.model_),
      fmi_(ap.fmi_),
      event_detector_(ap.event_params_),
      norm_(ap.model_, ap.evt_buffer_len_),
      seed_tracker_(ap.fmi_.size(),
                    ap.min_mean_conf_,
                    ap.min_top_conf_,
                    ap.min_aln_len_,
                    ap.seed_len_),
      state_(State::INACTIVE) {


    PathBuffer::MAX_PATH_LEN = opts_.seed_len_;

    for (u64 t = 0; t < EventType::NUM_TYPES; t++) {
        PathBuffer::TYPE_ADDS[t] = t << ((PathBuffer::MAX_PATH_LEN-2)*TYPE_BITS);
    }
    PathBuffer::TYPE_MASK = (u8) ((1 << TYPE_BITS) - 1);

    kmer_probs_ = std::vector<float>(model_.kmer_count());
    prev_paths_ = std::vector<PathBuffer>(opts_.max_paths_);
    next_paths_ = std::vector<PathBuffer>(opts_.max_paths_);
    sources_added_ = std::vector<bool>(model_.kmer_count(), false);

    prev_size_ = 0;
    event_i_ = 0;
    seed_tracker_.reset();
}

Mapper::Mapper(const Mapper &m) : Mapper(m.opts_) {}

Mapper::~Mapper() {
    for (u32 i = 0; i < next_paths_.size(); i++) {
        next_paths_[i].free_buffers();
        prev_paths_[i].free_buffers();
    }
}

ReadBuffer &Mapper::get_read() {
    return read_;
}

void Mapper::deactivate() {
    state_ = State::INACTIVE;
    reset_ = false;
}

/*
std::string Mapper::map_fast5(const std::string &fast5_name) {
    if (!fast5::File::is_valid_file(fast5_name)) {
        std::cerr << "Error: '" << fast5_name << "' is not a valid file \n";
    }

    ReadLoc aln;

    try {
        fast5::File file;
        file.open(fast5_name);
        
        if (file.is_open()) {  
            auto fast5_info = file.get_raw_samples_params();
            auto raw_samples = file.get_raw_samples();
            new_read(fast5_info.read_id, 0, fast5_info.read_number);
            aln = add_samples(raw_samples);

        } else {
            std::cerr << "Error: unable to open '" << fast5_name << "'\n";
        }

        
    } catch (hdf5_tools::Exception& e) {
        std::cerr << "Error: hdf5 exception '" << e.what() << "'\n";
    }

    return aln.str();
}

bool Mapper::add_sample(float s) {
    ;
    if (!event_detector_.add_sample(s)) return false;
    norm_.add_event(event_detector_.get_mean());
    float m = norm_.pop_event();

    #ifdef DEBUG_TIME
    read_loc_.sigproc_time_ += timer_.lap();
    #endif

    if (event_i_ >= opts_.max_events_proc_ || add_event(m)) {
        read_loc_.set_time(timer_.get());
        read_loc_.set_read_len(opts_, event_i_);
        return true;
    }

    return false;
}

ReadLoc Mapper::pop_loc() {
    state_ = State::INACTIVE;
    reset_ = false;
    return read_loc_;
}

ReadLoc Mapper::get_loc() const {
    return read_loc_;
}

ReadLoc Mapper::add_samples(const std::vector<float> &samples) {

    if (opts_.evt_buffer_len_ == 0) {
        #ifdef DEBUG_TIME
        timer_.reset();
        #endif

        std::vector<Event> events = event_detector_.add_samples(samples);
        std::vector<Event> old(events);
        model_.normalize(events);

        #ifdef DEBUG_TIME
        read_loc_.sigproc_time_ += timer_.lap();
        #endif

        read_loc_.set_read_len(opts_, events.size());

        for (u32 e = 0; e < events.size(); e++) {
            if (add_event(events[e].mean)) break;
        }
    } else {

        read_loc_.set_read_len(opts_, samples.size() / 5); //TODO: this better
        
        u32 i = 0;

        #ifdef DEBUG_TIME
        timer_.reset();
        #endif

        float m;
        for (auto s : samples) {
            if (!event_detector_.add_sample(s)) continue;
            norm_.add_event(event_detector_.get_mean());
            m = norm_.pop_event();

            #ifdef DEBUG_TIME
            read_loc_.sigproc_time_ += timer_.lap();
            #endif

            if (add_event(m)) break;
            i++;
        }
    }

    read_loc_.set_time(timer_.get());

    return read_loc_;
}
*/


void Mapper::new_read(Chunk &chunk) {
    if (prev_unfinished(chunk.get_number())) {
        std::cerr << "Error: possibly lost read '" << read_.id_ << "'\n";
    }

    read_ = ReadBuffer(chunk);
    prev_size_ = 0;
    event_i_ = 0;
    reset_ = false;
    last_chunk_ = false;
    state_ = State::MAPPING;
    seed_tracker_.reset();
    event_detector_.reset();
    norm_.skip_unread();
    timer_.reset();
}

u32 Mapper::prev_unfinished(u32 next_number) const {
    return state_ == State::MAPPING && read_.number_ != next_number;
}

bool Mapper::finished() const {
    return state_ == State::SUCCESS || state_ == State::FAILURE;
}

void Mapper::skip_events(u32 n) {
    event_i_ += n;
    prev_size_ = 0;
}

void Mapper::request_reset() {
    reset_ = true;
}

void Mapper::end_reset() {
    reset_ = false;
}

bool Mapper::is_resetting() {
    return reset_;
}

bool Mapper::is_chunk_processed() const {
    return read_.chunk_processed_;
}

Mapper::State Mapper::get_state() const {
    return state_;
}

bool Mapper::swap_chunk(Chunk &chunk) {
    //std::cout << "# tryna swap " << is_chunk_processed() << " " << reset_ << " " << state_ << "\n";
    if (!is_chunk_processed() || reset_) return false;

    //TODO: put in opts
    if (opts_.max_chunks_proc_ > 0 && read_.num_chunks_ == opts_.max_chunks_proc_) {
        state_ = State::FAILURE;
        reset_ = true;
        chunk.clear();
        return true;
    }

    bool added = read_.add_chunk(chunk);
    if (!added) std::cout << "# NOT ADDED " << chunk.get_id() << "\n";
    return added;
}

u16 Mapper::process_chunk() {
    if (read_.chunk_processed_ || reset_) return 0; 
    
    #ifdef DEBUG_TIME
    read_loc_.sigproc_time_ += timer_.lap();
    #endif

    float mean;

    u16 nevents = 0;
    for (u32 i = 0; i < read_.chunk_.size(); i++) {
        //std::cout << read_.chunk_[i] << "\n";
        if (event_detector_.add_sample(read_.chunk_[i])) {
            mean = event_detector_.get_mean();
            if (!norm_.add_event(mean)) {

                u32 nskip = norm_.skip_unread(nevents);
                skip_events(nskip);
                //TODO: report event skip in some way
                //std::cout << "# norm skipped " << nskip << "\n";
                if (!norm_.add_event(mean)) {
                    std::cerr << "# error: chunk events cannot fit in normilzation buffer\n";
                    return nevents;
                }
            }
            nevents++;
        }
    }

    //std::cout << "# processed " << channel_ << "\n";

    read_.chunk_.clear();
    read_.chunk_processed_ = true;
    return nevents;
}

bool Mapper::end_read(u32 number) {
    //set last chunk if you want to keep trying after read has ended
    //return last_chunk_ = (read_loc_.get_number() == number);
    return reset_ = (read_.number_ == number);
}

bool Mapper::map_chunk() {
    if (reset_ || (last_chunk_ && norm_.empty())) {
        state_ = State::FAILURE;
        return true;
    }
    u16 nevents = opts_.get_max_events(event_i_);
    float tlimit = opts_.evt_timeout_ * nevents;

    Timer t;
    for (u16 i = 0; i < nevents && !norm_.empty(); i++) {
        if (add_event(norm_.pop_event())) return true;
        if (t.get() > tlimit) {
            return false; //TODO: penalize this read
        }
    }

    return false;
}

bool Mapper::add_event(float event) {

    if (reset_ || event_i_ >= opts_.max_events_proc_) {
        reset_ = false;
        state_ = State::FAILURE;
        return true;
    }

    Range prev_range;
    u16 prev_kmer;
    float evpr_thresh;
    bool child_found;


    auto next_path = next_paths_.begin();

    for (u16 kmer = 0; kmer < model_.kmer_count(); kmer++) {
        kmer_probs_[kmer] = model_.event_match_prob(event, kmer);
    }
    #ifdef DEBUG_TIME
    read_loc_.prob_time_ += timer_.lap();
    #endif
    
    //Find neighbors of previous nodes
    for (u32 pi = 0; pi < prev_size_; pi++) {
        if (!prev_paths_[pi].is_valid()) {
            continue;
        }

        child_found = false;

        PathBuffer &prev_path = prev_paths_[pi];
        Range &prev_range = prev_path.fm_range_;
        prev_kmer = prev_path.kmer_;

        evpr_thresh = opts_.get_prob_thresh(prev_range.length());
        #ifdef DEBUG_TIME
        read_loc_.thresh_time_ += timer_.lap();
        #endif

        if (prev_path.consec_stays_ < opts_.max_consec_stay_ && 
            kmer_probs_[prev_kmer] >= evpr_thresh) {

            next_path->make_child(prev_path, 
                                  prev_range,
                                  prev_kmer, 
                                  kmer_probs_[prev_kmer], 
                                  EventType::STAY);

            child_found = true;

            if (++next_path == next_paths_.end()) {
                break;
            }
        }


        #ifdef DEBUG_TIME
        read_loc_.stay_time_ += timer_.lap();
        #endif

        //Add all the neighbors
        for (u8 b = 0; b < ALPH_SIZE; b++) {
            u16 next_kmer = model_.get_neighbor(prev_kmer, b);

            if (kmer_probs_[next_kmer] < evpr_thresh) {
                continue;
            }

            #ifdef DEBUG_TIME
            read_loc_.neighbor_time_ += timer_.lap();
            #endif

            Range next_range = fmi_.get_neighbor(prev_range, b);

            #ifdef DEBUG_TIME
            read_loc_.fmrs_time_ += timer_.lap();
            #endif

            if (!next_range.is_valid()) {
                continue;
            }

            next_path->make_child(prev_path, 
                                  next_range,
                                  next_kmer, 
                                  kmer_probs_[next_kmer], 
                                  EventType::MATCH);

            child_found = true;

            if (++next_path == next_paths_.end()) {
                break;
            }
        }

        #ifdef DEBUG_TIME
        read_loc_.neighbor_time_ += timer_.lap();
        #endif

        if (!child_found && !prev_path.sa_checked_) {

            update_seeds(prev_path, true);

        }

        if (next_path == next_paths_.end()) {
            break;
        }
    }

    if (next_path != next_paths_.begin()) {

        u32 next_size = next_path - next_paths_.begin();

        pdqsort(next_paths_.begin(), next_path);
        //std::sort(next_paths_.begin(), next_path);

        #ifdef DEBUG_TIME
        read_loc_.sort_time_ += timer_.lap();
        #endif

        u16 source_kmer;
        prev_kmer = model_.kmer_count(); 

        Range unchecked_range, source_range;

        for (u32 i = 0; i < next_size; i++) {
            source_kmer = next_paths_[i].kmer_;

            //Add source for beginning of kmer range
            if (source_kmer != prev_kmer &&
                next_path != next_paths_.end() &&
                kmer_probs_[source_kmer] >= opts_.get_source_prob()) {

                sources_added_[source_kmer] = true;

                source_range = Range(opts_.kmer_fmranges_[source_kmer].start_,
                                     next_paths_[i].fm_range_.start_ - 1);

                if (source_range.is_valid()) {
                    next_path->make_source(source_range,
                                           source_kmer,
                                           kmer_probs_[source_kmer]);
                    next_path++;
                }                                    

                unchecked_range = Range(next_paths_[i].fm_range_.end_ + 1,
                                        opts_.kmer_fmranges_[source_kmer].end_);
            }

            prev_kmer = source_kmer;

            //Range next_range = next_paths_[i].fm_range_;

            //Remove paths with duplicate ranges
            //Best path will be listed last
            if (i < next_size - 1 && next_paths_[i].fm_range_ == next_paths_[i+1].fm_range_) {
                next_paths_[i].invalidate();
                continue;
            }

            //Start source after current path
            //TODO: check if theres space for a source here, instead of after extra work?
            if (next_path != next_paths_.end() &&
                kmer_probs_[source_kmer] >= opts_.get_source_prob()) {
                
                source_range = unchecked_range;
                
                //Between this and next path ranges
                if (i < next_size - 1 && source_kmer == next_paths_[i+1].kmer_) {

                    source_range.end_ = next_paths_[i+1].fm_range_.start_ - 1;

                    if (unchecked_range.start_ <= next_paths_[i+1].fm_range_.end_) {
                        unchecked_range.start_ = next_paths_[i+1].fm_range_.end_ + 1;
                    }
                }

                //Add it if it's a real range
                if (source_range.is_valid()) {
                    next_path->make_source(source_range,
                                           source_kmer,
                                           kmer_probs_[source_kmer]);
                    next_path++;
                }
            }

            #ifdef DEBUG_TIME
            read_loc_.loop2_time_ += timer_.lap();
            #endif

            update_seeds(next_paths_[i], false);

        }
    }

    #ifdef DEBUG_TIME
    read_loc_.loop2_time_ += timer_.lap();
    #endif
    
    for (u16 kmer = 0; 
         kmer < model_.kmer_count() && 
            next_path != next_paths_.end(); 
         kmer++) {

        Range next_range = opts_.kmer_fmranges_[kmer];

        if (!sources_added_[kmer] && 
            kmer_probs_[kmer] >= opts_.get_source_prob() &&
            next_path != next_paths_.end() &&
            next_range.is_valid()) {

            //TODO: don't write to prob buffer here to speed up source loop
            next_path->make_source(next_range, kmer, kmer_probs_[kmer]);
            next_path++;

        } else {
            sources_added_[kmer] = false;
        }
    }

    #ifdef DEBUG_TIME
    read_loc_.source_time_ += timer_.lap();
    #endif

    prev_size_ = next_path - next_paths_.begin();
    prev_paths_.swap(next_paths_);

    //Update event index
    event_i_++;

    SeedGroup sg = seed_tracker_.get_final();

    if (sg.is_valid()) {
        state_ = State::SUCCESS;
        set_ref_loc(sg);

        #ifdef DEBUG_TIME
        read_loc_.tracker_time_ += timer_.lap();
        #endif

        return true;
    }

    #ifdef DEBUG_TIME
    read_loc_.tracker_time_ += timer_.lap();
    #endif

    return false;
}

void Mapper::update_seeds(PathBuffer &p, bool path_ended) {

    if (p.is_seed_valid(opts_, path_ended)) {

        #ifdef DEBUG_TIME
        read_loc_.tracker_time_ += timer_.lap();
        #endif

        p.sa_checked_ = true;

        for (u64 s = p.fm_range_.start_; s <= p.fm_range_.end_; s++) {

            //Reverse the reference coords so they both go L->R
            u64 ref_en = fmi_.size() - fmi_.sa(s) + 1;

            #ifdef DEBUG_TIME
            read_loc_.fmsa_time_ += timer_.lap();
            #endif

            seed_tracker_.add_seed(ref_en, p.match_len(), event_i_ - path_ended);

            #ifdef DEBUG_TIME
            read_loc_.tracker_time_ += timer_.lap();
            #endif

            #ifdef DEBUG_SEEDS
            seed.print(seeds_out);
            #endif
        }
    }

}

void Mapper::set_ref_loc(const SeedGroup &seeds) {
    u8 k_shift = (opts_.model_.kmer_len() - 1);

    bool fwd = seeds.ref_st_ > opts_.fmi_.size() / 2;

    u64 sa_st;
    if (fwd) sa_st = opts_.fmi_.size() - (seeds.ref_en_.end_ + k_shift);
    else      sa_st = seeds.ref_st_;
    
    std::string rf_name;
    u64 rd_len = (int) (450.0 * (read_.raw_len_ / 4000.0)), //TODO don't hard code
        rd_st = (u32) (opts_.max_stay_frac_ * seeds.evt_st_),
        rd_en = (u32) (opts_.max_stay_frac_ * (seeds.evt_en_ + opts_.seed_len_)) + k_shift,
        rf_st,
        rf_len = opts_.fmi_.translate_loc(sa_st, rf_name, rf_st), //sets rf_st
        rf_en = rf_st + (seeds.ref_en_.end_ - seeds.ref_st_) + k_shift;

    u16 match_count = seeds.total_len_ + k_shift;

    read_.loc_.set_mapped(rd_st, rd_en, rd_len, rf_name, rf_st, rf_en, rf_len, match_count, fwd);
}


