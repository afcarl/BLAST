/* imatsh application v 0.01 
 *
 * C++ prototype built on SoundSpotter's Matcher/MatchedFilter class
 *
 * LICENSE: Dartmouth College Commercial License
 *
 * Author: Michael A. Casey
 * Copyright (c) January 2010, Dartmouth College
 * All Rights Reserved
 *
 */

#include "aCollage.h"
#include "Compiler.h"

#define IMATSH_MAIN

void aCollage::error(const char* format, ... ){
  char buffer[MAXSTR];
  va_list args;
  va_start (args, format);
  vsprintf (buffer,format, args);
  fprintf (stderr, "%s\n", buffer);
  va_end (args);
  exit(EXIT_BAD);
}

bool operator< (NNresult const &a, NNresult const &b) {
  return a.query_pos < b.query_pos;
}

bool operator> (NNresult const &a, NNresult const &b) {
  return a.query_pos > b.query_pos;
}


aCollage::aCollage(const char* targetFile, 
	       const char* powerFile, 
	       const char* sourceListFile, 
	       const char* powerListFile, 
	       const char* mediaListFile,
	       int shingleSize, int hopSize, int queueSize, int loFeature, int hiFeature) :
  Matcher(), // default superclass constructor
  targetFeatures(0),
  targetPowers(0),
  sourceFeatures(0),
  sourcePowers(0),  
  track_offsets(0),
  media(0),
  matshup_set(0),
  shingleSize(shingleSize),
  hopSize(hopSize),
  queueSize(queueSize),
  loFeature(loFeature),
  hiFeature(hiFeature),
  featureDim(0)
{
  long queryLength = loadTarget(targetFile, powerFile);
  if( ! queryLength )
    error("Problem loading target");
  media = new std::vector<string> ();
  srand ( time(NULL) ); // initialize random seed for zero-feature initialization and random matching
  idxT dbLength = loadSources(sourceListFile, powerListFile, mediaListFile, targetFile);
  if( ! dbLength )
    error("Problem loading sources database");
  resize(shingleSize, dbLength);
  if( !(hiFeature && hiFeature<(int)featureDim) ){
    hiFeature=featureDim-1;
    fprintf(stderr,"*auto* hiFeature=%d\n", hiFeature);
  }
  aCollage::hiFeature = hiFeature;
  if(loFeature>hiFeature)
    error("loFeature %d > hiFeature %d", loFeature, hiFeature);
  computeSeriesMeanPowers();
  updateDatabaseNorms(sourceFeatures, shingleSize, sourceFeatures->getCols(), loFeature, hiFeature);
  matshup_set = new MatshupSet ();
}

aCollage::~aCollage(){
  delete media;
  delete targetFeatures;
  delete targetPowers;
  delete sourceFeatures;
  delete sourcePowers;  
  delete track_offsets;
  delete matshup_set;
}

long aCollage::loadTarget(const char* targetFile, const char* targetPowerFile){
  long numVectors = countVectors(targetFile);
  if( ! numVectors )
    error("Problem counting target vectors in %s.", targetFile);
  targetFeatures = new SeriesOfVectors(featureDim, numVectors);
  targetPowers = new SeriesOfVectors(1, numVectors);
  long loadedVectors = loadVectors(targetFeatures, targetFile, featureDim);
  if( ! (loadedVectors && loadedVectors == numVectors) )
    error("Problem loading target vectors from %s.", targetFile);
  loadedVectors = loadVectors(targetPowers, targetPowerFile, 1);
  if( ! (loadedVectors && loadedVectors == numVectors) )
    error("Problem loading target powers from %s.", targetPowerFile);
  return numVectors;
}

long aCollage::loadSources(const char* fileName, const char* powerName, 
			 const char* mediaName, const char* targetName){
  ifstream sourceList (fileName);
  if( ! ( sourceList && sourceList.good() ) )
    return 0;
  ifstream powerList (powerName);
  if( ! ( powerList && powerList.good() ) )
    return 0;
  ifstream mediaList (mediaName);
  if( ! ( mediaList && mediaList.good() ) )
    return 0;
  char currentFileName[MAXSTR];
  char currentPowerName[MAXSTR];
  idxT numVectors, numPowers, totalVectors = 0, numFiles=0;
  while( sourceList.good() ){
    sourceList.getline(currentFileName, MAXSTR);
    if( !sourceList.good())
      break;
    if(!SKIP_TARGET_IDENTITY || strncmp(currentFileName,targetName,MAXSTR)){
	numVectors = countVectors(currentFileName);
	if( ! numVectors )
	  error("Problem counting source vectors in %s", currentFileName);
	totalVectors += numVectors;
	numFiles++;
      }
  }
  sourceFeatures = new SeriesOfVectors(featureDim, totalVectors);
  sourcePowers = new SeriesOfVectors(1, totalVectors);
  if( ! ( sourceFeatures || sourcePowers) )
    error("Cannot allocate source features/powers of size: %d", totalVectors);
  track_offsets = new std::vector<off_t>;
  if( ! track_offsets )
    error("Could not allocate track_offsets");
  track_offsets->reserve(numFiles);
  sourceList.close();
  sourceList.open(fileName, ifstream::in);
  totalVectors = 0;  

  char m_str[MAXSTR];
  while( sourceList.good() ){
    sourceList.getline(currentFileName, MAXSTR);
    powerList.getline(currentPowerName, MAXSTR);
    mediaList.getline(m_str, MAXSTR);
    string currentMedia(m_str);
    if( ! (sourceList.good() && powerList.good() && mediaList.good() ) )
      break;
    if(SKIP_TARGET_IDENTITY && strncmp(currentFileName,targetName,MAXSTR)==0){
      fprintf(stderr,"Skipping identity file: %s\n", currentFileName);
      fflush(stderr);
    }
    else{
	numVectors = loadVectors(sourceFeatures, currentFileName, featureDim, (off_t)totalVectors);
	numPowers = loadVectors(sourcePowers, currentPowerName, 1, (off_t)totalVectors);
	media->push_back(currentMedia);
	if( ! ( numVectors || numPowers) || ( numVectors!=numPowers ) )
	  error("Problem loading source vectors/powers at sourceList=%s", currentFileName);
	track_offsets->push_back((off_t)totalVectors);
	totalVectors += numVectors;
    }
  }
  track_offsets->push_back((off_t)totalVectors);
  sourceList.close();
  powerList.close();
  if( totalVectors != sourceFeatures->getCols() )
    error("Mismatch between counted and loaded source vectors");
  fprintf(stderr, "Loaded %d source files, %d vectors, total memory = %d Kb\n", 
	  (int)numFiles, (int)totalVectors, (int)((totalVectors*featureDim*sizeof(ss_sample))/1024));
  return totalVectors;
}

long aCollage::countVectors(const char* fileName){
  long begin, end;
  int dim;
  fprintf(stderr,"Counting vectors in %s...\n", fileName);
  fflush(stderr);
  ifstream currentFile (fileName, ifstream::binary);
  if(! currentFile )
    error("Cannot open %s for counting...", fileName);
  if( ! currentFile.good() )
    return 0;
  currentFile.read((char*)&dim, sizeof(int));
  if( ! featureDim )
    featureDim=dim; // assign on first load
  if( (idxT)dim != featureDim )
    error("Loaded feature set dimension mismatch %d!=%d\n", dim, featureDim);
  begin = currentFile.tellg();
  currentFile.seekg (0, ios::end);
  end = currentFile.tellg();
  currentFile.close();
  return ( end - begin ) / (featureDim*sizeof(ff_sample));
}


long aCollage::loadVectors(SeriesOfVectors* s, const char* fileName, idxT dimension, off_t vectorOffset){
  ifstream loader(fileName, ifstream::binary);
  if( ! ( loader && loader.good() ) )
    return 0;
  int dim;
  idxT numRead=0;
  loader.read((char*)&dim, sizeof(int));
  if( (idxT)dim != dimension )
    fprintf(stderr, "Loaded feature set dimension mismatch %d!=%d\n", (int)dim, (int)dimension);
  ff_sample buf[dimension];
  float twoOverMaxInt = 2.0f / MAXRANDINT;
  while( loader.good() && vectorOffset + numRead < s->getCols() ){
    loader.read((char*) buf, dimension * sizeof(ff_sample) );
    if(!loader.good())
      break ;    
    ss_sample* fbuf = s->getCol( vectorOffset + numRead ) ;
    ff_sample* dbuf = buf;
    int l = dimension;
    while( l--){
      if( ISNAN(*dbuf) || *dbuf < -1e6 ){
	*fbuf++ = rand() * twoOverMaxInt - 1.0f ; // randomly map Nan and -Inf
	dbuf++;
      }
      else{
	*fbuf++ = (float) (*dbuf++); // type conversion from ff_sample
      }
    }
    numRead++;
  }
  loader.close();
  fprintf(stderr, "Loaded %s, %u vectors of dim %d\n", fileName, (unsigned int)numRead, (unsigned int)dimension);
  fflush(stdout);
  return numRead; // number of observations read plus countSoFar
}

void aCollage::computeSeriesMeanPowers(){
  targetPowers->seriesMean(targetPowers->getSeries(), shingleSize, targetPowers->getCols());
  int k, numFiles = track_offsets->size() - 1;     
  for(k = 0; k < numFiles ; k++ )
    sourcePowers->seriesMean(sourcePowers->getCol(track_offsets->at(k)), 
			     shingleSize, 
			     track_offsets->at(k+1)-track_offsets->at(k));  
}

inline uint32_t index_to_track_id(std::vector<off_t>* track_offsets, uint32_t imatsh_id){
  std::vector<off_t>::iterator b = track_offsets->begin();
  std::vector<off_t>::iterator e = track_offsets->end();  
  std::vector<off_t>::iterator p = upper_bound(b, e, (off_t) imatsh_id);
  return p - b - 1;
}

inline uint32_t index_to_track_pos(std::vector<off_t>* track_offsets, uint32_t track_id, uint32_t imatsh_id) {
  uint32_t trackIndexOffset = (*track_offsets)[track_id];
  return imatsh_id - trackIndexOffset;
}

int aCollage::doMatshup(){
  idxT qpoint, muxi;
  idxT dbSize = sourceFeatures->getCols();
  SeriesOfVectors inShingle(featureDim, shingleSize);
  uint32_t match_id, track_id;
  NNresult result;

  for( qpoint = 0 ; qpoint < targetFeatures->getCols()-shingleSize ; qpoint+=hopSize ){
    for( muxi = 0 ; muxi < shingleSize ; muxi++ ){
      inShingle.setCol(muxi, targetFeatures->getCol(qpoint+muxi));
      insert(&inShingle, shingleSize, sourceFeatures, dbSize, muxi, loFeature, hiFeature);
      if(muxi==shingleSize-1){
	result.query_pos = qpoint;
	match_id = match(0.0f, shingleSize, dbSize, 0, 0, queueSize, 
				  *targetPowers->getCol(qpoint), sourcePowers->getSeries(), 
				  -4.5f, 0.0f, track_offsets);
	track_id = index_to_track_id(track_offsets, match_id);
	result.track_pos = index_to_track_pos(track_offsets, track_id, match_id);
	result.media_idx = track_id;
	result.media = (*media)[track_id];
	result.dist = getDist();
	matshup_set->insert(result);
      }
    }
  }
  return EXIT_GOOD;
}

// record top N matches per query point
int aCollage::doMultaCollageup(uint32_t numHits, int randomMatch){
  idxT qpoint, muxi;
  idxT dbSize = sourceFeatures->getCols();
  SeriesOfVectors inShingle(featureDim, shingleSize);
  uint32_t track_id;
  NNresult result;
  MatchResult thisMatch;
  ResultQueue *matshupQueue = 0;
  float oneOverMaxInt = 1.0f / MAXRANDINT;
  if(numHits==0)
    aCollage::error("numHits==0, in doMultaCollageup(numHits)");
  for( qpoint = 0 ; qpoint < targetFeatures->getCols() - shingleSize + 1; qpoint+=hopSize ){
    for( muxi = 0 ; muxi < shingleSize ; muxi++ ){
      inShingle.setCol(muxi, targetFeatures->getCol(qpoint+muxi));
      insert(&inShingle, shingleSize, sourceFeatures, dbSize, muxi, loFeature, hiFeature);
      if(muxi==shingleSize-1){
	matshupQueue = new ResultQueue();
	result.query_pos = qpoint;
	multiMatch(matshupQueue, numHits, 0.0f, shingleSize, dbSize, 0, 0, queueSize, 
		   *targetPowers->getCol(qpoint), sourcePowers->getSeries(), 
		   -4.5f, 0.0f, track_offsets);
	uint32_t N = numHits;
	while( ! matshupQueue->empty() && N--){
	  thisMatch = matshupQueue->top();
	  matshupQueue->pop(); 
	  if (randomMatch)
	    thisMatch.track_pos = (uint32_t)(rand() * oneOverMaxInt * (dbSize-1));
	  track_id = index_to_track_id(track_offsets, thisMatch.track_pos);
	  result.track_pos = index_to_track_pos(track_offsets, track_id, thisMatch.track_pos);
	  result.media_idx = track_id;
	  result.media = (*media)[track_id];
	  result.dist = thisMatch.dist;
	  fprintf(stdout, "%f %f %f %d %d %d\n", 
		  fabs(result.dist), 0., 0., result.query_pos, result.track_pos, result.media_idx);
	  matshup_set->insert(result);
	}
	delete matshupQueue;    
      }
    }
  }
  return EXIT_GOOD;
}

MatshupSet* aCollage::getResultSet(){
  return matshup_set;
}

#ifdef IMATSH_MAIN

int processArguments(int argc, const char* argv[],  
		     const char **targetFeatures, const char **targetPowers, const char **targetFileName,
		     const char **sourceFeatureList, const char **sourcePowerList, 
		     const char **mediaList,
		     int* shingleSize, int* hopSize, int* queueSize, int* loFeature, int* hiFeature, float* beta, float* mix, int* numHits, int* frameSizeInSamples, int* frameHopInSamples, int* randomMatch){

  *loFeature = *hiFeature = 0;
  int arg = 0;

  // COMPILE ONLY MODE, OPERATE ON MATCH FILE
  if(argc==9){
    fprintf(stderr, "Using compile only mode...\n");
    *targetFeatures = argv[++arg]; // matshup file
    *targetFileName = argv[++arg]; // target file
    *mediaList = argv[++arg];
    _get_arg(argc, argv, &arg, shingleSize, "shingleSize");
    _get_arg(argc, argv, &arg, hopSize, "hopSize");
    _get_arg(argc, argv, &arg, beta, "beta");  
    _get_arg(argc, argv, &arg, frameSizeInSamples, "frameSizeInSamples");
    _get_arg(argc, argv, &arg, frameHopInSamples, "frameHopInSamples");
  }
  // MATCH AND COMPILE MODE
  else if(argc>9){
    *targetFeatures = argv[++arg];
    *targetPowers = argv[++arg];
    *targetFileName = argv[++arg];
    *sourceFeatureList = argv[++arg];
    *sourcePowerList = argv[++arg];
    *mediaList = argv[++arg];
    _get_arg(argc, argv, &arg, shingleSize, "shingleSize");
    _get_arg(argc, argv, &arg, hopSize, "hopSize");
    _get_arg(argc, argv, &arg, queueSize, "queueSize");
    _get_arg(argc, argv, &arg, loFeature, "loFeature");
    _get_arg(argc, argv, &arg, hiFeature, "hiFeature");  
    _get_arg(argc, argv, &arg, beta, "beta");  
    _get_arg(argc, argv, &arg, numHits, "numHits");  
    _get_arg(argc, argv, &arg, frameSizeInSamples, "frameSizeInSamples");
    _get_arg(argc, argv, &arg, frameHopInSamples, "frameHopInSamples");
    _get_arg(argc, argv, &arg, randomMatch, "randomMatch");
  }
  else{
    fprintf(stderr, "Usage: aCollage target.features target.powers targetFilename.wav sourceFeatureList.txt sourcePowerList.txt MediaList.txt shingleSize hopSize [queueSize loFeature hiFeature beta numHits frameSizeInSamples frameHopInSamples randomMatch{0,1, <0 matchOnly}]\n");
    return EXIT_BAD;
  }
  // AUTO SET MIX
  if(shingleSize){      
    if(*mix<std::numeric_limits<float>::epsilon()){
      *mix = 1.0f/((float)*shingleSize/(float)*hopSize); // Automatically set to 1/sqrt(W/H)
      fprintf(stderr, "mix=%f\n", *mix);
    }
    return EXIT_GOOD;
  }
  else{
    fprintf(stderr, "Bad shingle size in compile-only call.\n");
    return EXIT_BAD;
  }
}

void _get_arg(int argc, const char* argv[], int* c, int* a, const char* str){ // get int argument
  *c = (*c)+1;
  if(argc > *c){
    *a = atoi(argv[*c]);
    fprintf(stderr, "%s=%d\n", str, *a);
  }
}

void _get_arg(int argc, const char* argv[], int* c, float* a, const char* str){ // get float argument
  *c = (*c)+1;
  if(argc > *c){
    *a = atof(argv[*c]);
    fprintf(stderr, "%s=%f\n", str, *a);
  }  
}

int loadMatshupSet(MatshupSet* matshup, const char *matshupFileName, const char* mediaListName){
  ifstream matshupFile(matshupFileName);
  NNresult r;
  float dummy;
  vector<string> mediaList;
  string s;
  if(!matshupFile)
    aCollage::error("Cannot open aCollage file %s for reading.", matshupFileName);


  ifstream mediaListFile (mediaListName);
  if( ! ( mediaListFile && mediaListFile.good() ) )
    return 0;
  while(mediaListFile.good()){
    std::getline(mediaListFile,s);
    if(!mediaListFile.eof())
      mediaList.push_back(s);
  }

  while(matshupFile.good()){
    matshupFile >> r.dist;
    if(!matshupFile.good())
      break;
    matshupFile >> dummy;
    matshupFile >> dummy;
    matshupFile >> r.query_pos;
    matshupFile >> r.track_pos;
    matshupFile >> r.media_idx;
    if(r.media_idx < mediaList.size())
      r.media = mediaList[r.media_idx];
    else{
      fprintf(stderr,"Error: source media index %d is out of range [%d]\n", r.media_idx,(int)mediaList.size());
      return EXIT_BAD;
    }
    matshup->insert(r);
  }
  matshupFile.close();
  return EXIT_GOOD;
}

int main(int argc, const char* argv[]){
  const char *targetFeatures=0, *targetPowers=0;
  const char *sourceFeatureList=0, *sourcePowerList=0;
  const char *targetFileName=0, *mediaList=0;
  int shingleSize=0, hopSize=0, queueSize=0, loFeature=0, hiFeature=0, numHits=0, frameSizeInSamples=IMATSH_DEFAULT_FRAME_SIZE, frameHopInSamples=IMATSH_DEFAULT_FRAME_HOP, randomMatch=0;
  float beta = 3.0, mix = 0.0;
  MatshupSet* matshup;
  bool matchOnly = false;
  int retval = processArguments(argc, argv, &targetFeatures, &targetPowers, &targetFileName, &sourceFeatureList, 
				&sourcePowerList, &mediaList, &shingleSize, &hopSize, &queueSize, &loFeature, &hiFeature, &beta, &mix, &numHits,&frameSizeInSamples,&frameHopInSamples,&randomMatch);
  if ( retval == EXIT_BAD)
    exit(EXIT_BAD);

  if(argc>9){
    aCollage* app = new aCollage(targetFeatures, targetPowers, sourceFeatureList, sourcePowerList, 
			     mediaList, shingleSize, hopSize, queueSize, loFeature, hiFeature);
    if(randomMatch<0){ // punning the randomMatch field for matchOnly mode
      randomMatch=0;
      matchOnly=true;
    }
    app->doMultaCollageup(numHits, randomMatch);
    matshup = new MatshupSet(*app->getResultSet());
    delete app;
  }
  else{
    const char* matshupFile = targetFeatures; // punning targetFeatures for matshupFile
    matshup = new MatshupSet();
    if(loadMatshupSet(matshup, matshupFile, mediaList)==EXIT_BAD)
      exit(EXIT_BAD);
  }  

  if(!matchOnly){
  fprintf(stderr,"Compiling: targetFeatures=%s, targetFileName=%s, shingleSize=%d, hopSize=%d, beta=%3.2f, mix=%3.2f, frameSizeInSamples=%d, frameHopInSamples=%d\n", 
	  targetFeatures, targetFileName, shingleSize, hopSize, beta, mix, frameSizeInSamples, frameHopInSamples);  
  Compiler* output = new Compiler(matshup, targetFileName, shingleSize, hopSize, beta, mix, 
				  frameSizeInSamples, frameHopInSamples, targetFeatures); // added targetFeatures mkc 3/20/17
  while ( ! output->empty() )
    output->compile_next();
  output->complete();
  delete output;  
  }

  delete matshup;

  return EXIT_GOOD;
}

#endif
