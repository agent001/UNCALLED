LIBS=-lz -lm -lstdc++ -ldl
HDF5_LIB=-L/cm/shared/apps/hdf5/1.8.17/lib /cm/shared/apps/hdf5/1.8.17/lib/libhdf5.a
BOOST_INCLUDE=-I/cm/local/apps/boost/1.58.0/include
HDF5_INCLUDE=-I/cm/shared/apps/hdf5/1.8.17/include
CC=g++
CFLAGS=-Wall -std=c++11 -O3
INCLUDE=-I./src/fast5/src -I./src/scrappie -I./src ${BOOST_INCLUDE}

all: uncalled 

%.o: %.cpp
	$(CC) $(CFLAGS) -c -o $@ $^ $(INCLUDE) $(HDF5_INCLUDE) 


#sigalign: sigalign.o nano_fmi.o kmer_model.o seed_graph.o
#	$(CC) $(CFLAGS) nano_fmi.o kmer_model.o seed_graph.o sigalign.o -o sigalign $(INCLUDE) $(HDF5_INCLUDE) $(HDF5_LIB) $(LIBS) 

uncalled: uncalled.o nano_fmi.o kmer_model.o seed_graph.o seed_tracker.o arg_parse.o
	$(CC) $(CFLAGS) nano_fmi.o kmer_model.o seed_graph.o seed_tracker.o arg_parse.o uncalled.o -o uncalled $(INCLUDE) $(HDF5_INCLUDE) $(HDF5_LIB) $(LIBS) 

#seed_tracker_test: seed_tracker_test.o kmer_model.o seed_tracker.o seed_graph.o nano_fmi.o 
#	$(CC) $(CFLAGS) seed_tracker_test.o kmer_model.o nano_fmi.o seed_tracker.o seed_graph.o -o seed_tracker_test $(INCLUDE) $(HDF5_INCLUDE) $(HDF5_LIB) $(LIBS) 

#arg_parse_test: 
#	$(CC) $(CFLAGS) arg_parse.o -o arg_parse_test $(INCLUDE) $(LIBS)

clean:
	rm *.o
