#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>

#include "rlcsa.h"
#include "misc/utils.h"


using namespace CSA;


int main(int argc, char** argv)
{
  std::cout << "RLCSA LCP test" << std::endl;
  if(argc < 5)
  {
    std::cout << "Usage: lcp_test basename queries runs modes [random_seed]" << std::endl;
    std::cout << std::endl;
    std::cout << "Supported modes:" << std::endl;
    std::cout << "d -- Direct LCP" << std::endl;
    std::cout << "p -- PLCP" << std::endl;
    std::cout << "s -- Sampled LCP" << std::endl;
    std::cout << "h -- Hybrid: PLCP and Sampled LCP" << std::endl;
    std::cout << "l -- Locate" << std::endl;
    std::cout << "v -- Verify results" << std::endl;
    std::cout << std::endl;
    return 1;
  }

  std::string base_name = argv[1];
  std::cout << "Base name: " << base_name << std::endl;
  usint queries = atoi(argv[2]);
  std::cout << "Number of queries: " << queries << std::endl;
  usint runs = std::max(1, atoi(argv[3]));
  std::cout << "Number of test runs: " << runs << std::endl;

  std::string mode_string(argv[4]);
  usint modes = 0;
  bool mode_direct  = (mode_string.find('d') != std::string::npos);
  bool mode_plcp    = (mode_string.find('p') != std::string::npos);
  bool mode_sampled = (mode_string.find('s') != std::string::npos);
  bool mode_hybrid  = (mode_string.find('h') != std::string::npos);
  bool mode_locate  = (mode_string.find('l') != std::string::npos);
  bool mode_verify  = (mode_string.find('v') != std::string::npos);
  std::cout << "Modes: ";
  if(mode_direct)  { std::cout << "direct "; modes++; }
  if(mode_plcp)    { std::cout << "plcp "; modes++; }
  if(mode_sampled) { std::cout << "sampled "; modes++; }
  if(mode_hybrid)  { std::cout << "hybrid "; modes++; }
  if(mode_locate)  { std::cout << "locate "; }
  if(mode_verify)  { std::cout << "verify"; }
  std::cout << std::endl;

  usint seed = 0xDEADBEEF;
  if(argc > 5)
  {
    seed = atoi(argv[5]);
  }
  std::cout << "Random seed: " << seed << std::endl; 
  std::cout << std::endl;
  if(modes == 0 || queries == 0) { return 0; }

  RLCSA rlcsa(base_name);
  if((mode_plcp || mode_hybrid) && !rlcsa.supportsLocate())
  {
    std::cerr << "Error: Locate is not supported!" << std::endl;
    return 2;
  }
  rlcsa.printInfo();
  rlcsa.reportSize(true);

  RLEVector* plcp = 0;
  if(mode_plcp || mode_hybrid)
  {
    std::string plcp_name = base_name + PLCP_EXTENSION;
    std::ifstream plcp_file(plcp_name.c_str(), std::ios_base::binary);
    if(!plcp_file)
    {
      std::cerr << "Error: Cannot open PLCP file!" << std::endl;
      return 3;
    }
    plcp = new RLEVector(plcp_file);
    std::cout << "PLCP:            " << (plcp->reportSize() / (double)MEGABYTE) << " MB" << std::endl;
    plcp_file.close();
  }

  LCPSamples* lcp = 0;
  if(mode_sampled)
  {
    std::string lcp_name = base_name + LCP_SAMPLES_EXTENSION;
    std::ifstream lcp_file(lcp_name.c_str(), std::ios_base::binary);
    if(!lcp_file)
    {
      std::cerr << "Error: Cannot open LCP sample file!" << std::endl;
      delete plcp;
      return 4;
    }
    lcp = new LCPSamples(lcp_file);
    std::cout << "Sampled LCP:     " << (lcp->reportSize() / (double)MEGABYTE) << " MB" << std::endl;
    lcp_file.close();
  }

  LCPSamples* minimal = 0;
  if(mode_hybrid)
  {
    std::string minimal_name = base_name + ".minimal";
    std::ifstream minimal_file(minimal_name.c_str(), std::ios_base::binary);
    if(!minimal_file)
    {
      std::cerr << "Error: Cannot open minimal LCP sample file!" << std::endl;
      delete plcp; delete lcp;
      return 5;
    }
    minimal = new LCPSamples(minimal_file);
    std::cout << "Minimal samples: " << (minimal->reportSize() / (double)MEGABYTE) << " MB" << std::endl;
    minimal_file.close();
  }
  std::cout << std::endl;

  srand(seed);
  usint* positions = new usint[queries];
  usint* results1 = new usint[queries];
  usint* results2 = new usint[queries];
  usint* results3 = new usint[queries];
  usint* results4 = new usint[queries];
  usint* results5 = new usint[queries];
  for(usint i = 0; i < queries; i++)
  {
    positions[i] = rand() % rlcsa.getSize();
  }
  double start, time;

  if(mode_direct)
  {
    std::cout << "Direct LCP computation:" << std::endl;
    for(usint j = 0; j < runs; j++)
    {
      start = readTimer();
      for(usint i = 0; i < queries; i++)
      {
        results1[i] = rlcsa.lcpDirect(positions[i]);
      }
      time = readTimer() - start;
      std::cout << queries << " queries in " << time << " seconds (" << (queries / time) << " / s)" << std::endl;
    }
    std::cout << std::endl;
  }

  if(mode_plcp)
  {
    std::cout << "Using PLCP:" << std::endl;
    RLEVector::Iterator iter(*plcp);
    for(usint j = 0; j < runs; j++)
    {
      start = readTimer();
      for(usint i = 0; i < queries; i++)
      {
        usint pos = rlcsa.locate(positions[i]);
        results2[i] = iter.select(pos) - 2 * pos;
      }
      time = readTimer() - start;
      std::cout << queries << " queries in " << time << " seconds (" << (queries / time) << " / s)" << std::endl;
    }
    std::cout << std::endl;
  }

  if(mode_sampled)
  {
    std::cout << "Using Sampled LCP:" << std::endl;
    for(usint j = 0; j < runs; j++)
    {
      start = readTimer();
      for(usint i = 0; i < queries; i++)
      {
        results3[i] = rlcsa.lcp(positions[i], *lcp);
      }
      time = readTimer() - start;
      std::cout << queries << " queries in " << time << " seconds (" << (queries / time) << " / s)" << std::endl;
    }
    std::cout << std::endl;
  }

  if(mode_hybrid)
  {
    std::cout << "Using hybrid approach:" << std::endl;
    for(usint j = 0; j < runs; j++)
    {
      start = readTimer();
      for(usint i = 0; i < queries; i++)
      {
        results4[i] = rlcsa.lcp(positions[i], *minimal, *plcp);
      }
      time = readTimer() - start;
      std::cout << queries << " queries in " << time << " seconds (" << (queries / time) << " / s)" << std::endl;
    }
    std::cout << std::endl;
  }

  if(mode_locate)
  {
    std::cout << "Locate:" << std::endl;
    for(usint j = 0; j < runs; j++)
    {
      start = readTimer();
      for(usint i = 0; i < queries; i++)
      {
        results5[i] = rlcsa.locate(positions[i]);
      }
      time = readTimer() - start;
      std::cout << queries << " queries in " << time << " seconds (" << (queries / time) << " / s)" << std::endl;
    }
    std::cout << std::endl;
  }

  if(mode_verify && modes > 1)
  {
    for(usint i = 0; i < queries; i++)
    {
      bool ok = true;
      ok &= !mode_direct  | !mode_plcp    | results1[i] == results2[i];
      ok &= !mode_direct  | !mode_sampled | results1[i] == results3[i];
      ok &= !mode_direct  | !mode_hybrid  | results1[i] == results4[i];
      ok &= !mode_plcp    | !mode_sampled | results2[i] == results3[i];
      ok &= !mode_plcp    | !mode_hybrid  | results2[i] == results4[i];
      ok &= !mode_sampled | !mode_hybrid  | results3[i] == results4[i];
      if(!ok)
      {
        std::cout << "Query " << i << ": LCP[" << positions[i] << "] = ";
        if(mode_direct)  { std::cout << results1[i] << " (direct) ";  }
        if(mode_plcp)    { std::cout << results2[i] << " (plcp) ";    }
        if(mode_sampled) { std::cout << results3[i] << " (sampled) "; }
        if(mode_hybrid)  { std::cout << results4[i] << " (hybrid) ";  }
        std::cout << std::endl;
      }
    }
  }

  delete[] positions;
  delete[] results1; delete[] results2; delete[] results3; delete[] results4; delete[] results5;
  delete plcp; delete lcp; delete minimal;
  return 0;
}
