#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <string>
#include <iomanip>
#include <fstream>
#include <math.h>
#include <complex>
#include <algorithm>
#include <string>
#include <vector>
#include <unistd.h>
using namespace std;

/**** samtools headers ****/
#include <bam.h>
#include <sam.h>

/**** user headers ****/
#include "samfunctions.h"
#include "functions.h"
#include "matchreads.h"
#include "pairguide.h"

#include "preprocess.h"

///*! @abstract defautl mask for pileup */
//#define BAM_DEF_MASK (BAM_FUNMAP | BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP)

// default filter for mpileup and map quality
bool is_read_count_for_depth(const bam1_t *b) 
{
  if ( b->core.flag & BAM_DEF_MASK ) return false;;
  if ( (int)b->core.qual < msc::minMAPQ ) return false;
  return true;
}
bool is_read_count_for_depth(const bam1_t *b, int qual) 
{
  if ( b->core.flag & BAM_DEF_MASK ) return false;;
  if ( (int)b->core.qual < qual ) return false;
  return true;
}

// default filter for mpileup and map quality
// require FR orientation
bool is_read_count_for_pair(const bam1_t *b) 
{
  if ( b->core.flag & BAM_DEF_MASK ) return false;;
  if ( (int)b->core.qual < msc::minMAPQ ) return false;
  if ( b->core.mtid != b->core.tid )  return false;
  if ( bool(b->core.flag & BAM_FREVERSE) == 
       bool(b->core.flag & BAM_FMREVERSE) ) return false;
  return true;
}

//! decide if a read should be included for possible softclip matching
bool is_keep_read(const bam1_t *b, string& FASTA, RSAI_st& iread )
{
  if ( ! is_read_count_for_depth(b) ) return false;
  if ( (int)b->core.qual < msc::minMAPQ ) return false;
  if ( (int)b->core.n_cigar <=1 ) return false;
  if ( (int)b->core.tid < 0 ) return false;
  if ( b->core.tid != msc::bam_ref ) cerr << "#TARGET read error" << endl;
  
  POSCIGAR_st bm;
  resolve_cigar_pos(b, bm, 0);  
  
  // S part before the start of reference
  if ( bm.cop[0] < 0 || bm.cop.back()+bm.nop.back()>=(int)FASTA.size()  ) return false;
  if ( bm.cop[0]+bm.nop[0] < bm.cop[0] ) return false;
  if ( bm.pos<=0 ) return false;
  if ( bm.iclip<0 ) return false;
  if ( (int)bm.nop[bm.iclip] < msc::minSNum ) return false;
  
  // minumum base quality
  uint8_t *t = bam1_qual(b);
  if ( msc::minBASEQ>1 && t[0] != 0xff) {
    int bQ=0, bQCount=0;
    for(int i=0;i<(int)bm.nop[bm.iclip];++i) {
      bQ=t[ bm.qop[bm.iclip]+i ];
      if ( bQ < msc::minBASEQ ) ++bQCount;
    }
    if ( bQCount*4 > (int)bm.nop[bm.iclip] ) return false;
  }
  
  string SEQ=get_qseq(b);  
  
  // S part beyond reference
  if ( bm.cop.back()+bm.nop.back() >= (int)FASTA.size() ) return false;
  if ( bm.cop[0]<0 ) return false;
  
  //int nAdjust=calibrate_resolved_cigar_pos(FASTA, SEQ, bm);  
  //if ( msc::verbose && nAdjust>50 ) cerr << "nAdjust=" << nAdjust << endl;
  calibrate_resolved_cigar_pos(FASTA, SEQ, bm);  
  
  // short or no S part
  if ( bm.pos==0 ) return false;
  if ( bm.iclip<0 ) return false;
  if ( (int)bm.nop[bm.iclip] < msc::minSNum ) return false;
  
  // S part too long
  if ( bm.nop[bm.iclip]*1.25 > bm.l_qseq ) return false;
  //if ( bm.nop[bm.iclip]*2 > bm.l_qseq ) return false;
  
  int ndiff_m=0;
  for (int k = 0; k < (int) bm.op.size(); ++k) {
    if ( bm.op[k]!=BAM_CMATCH && bm.op[k]!=BAM_CEQUAL ) continue; 
    string CLIPPEDSEQ=SEQ.substr(bm.qop[k], bm.nop[k]);
    string REFCLIP=FASTA.substr(bm.cop[k], bm.nop[k]);
    for(int i=0; i<(int)REFCLIP.size(); ++i) if ( REFCLIP[i]!=CLIPPEDSEQ[i] ) ++ndiff_m;
  }
  int ndiff_s=0, nN=0;
  if ( bm.iclip>=0 ) {
    int k = bm.iclip;
    string CLIPPEDSEQ=SEQ.substr(bm.qop[k], bm.nop[k]);
    string REFCLIP=FASTA.substr(bm.cop[k], bm.nop[k]);
    for(int i=0; i<(int)REFCLIP.size(); ++i) {
      if ( REFCLIP[i]!=CLIPPEDSEQ[i] ) ++ndiff_s;
      if ( CLIPPEDSEQ[i]=='N' ) ++nN;
	}
  }
  // too few different bases in S part
  if ( ndiff_s <= 2 ) return false;    
  if ( ndiff_s <= (int)bm.nop[bm.iclip]/4 ) return false;
  // too many different bases in M part
  if ( ndiff_m >= (int)bm.l_qseq*8/100  ) return false;
  // too many no-call bases
  if ( nN >= msc::minSNum/2 ) return false;
  
  int nS=0,nIndel=0;
  for(int i=0;i<(int)bm.op.size();++i) {
    if (bm.op[i]==BAM_CSOFT_CLIP ) ++nS;
    if (bm.op[i]==BAM_CINS || bm.op[i]==BAM_CDEL || bm.op[i]==BAM_CREF_SKIP || bm.op[i]==BAM_CPAD ) ++nIndel;
  }
  // CIGAR too complicated
  if ( nS>3 || nIndel>3 ) return false;    
  
  //  int Snum_adjust=bm.nop[bm.iclip];
  int Sotherend=0;
  if ( bm.iclip==0 && bm.op.back()==BAM_CSOFT_CLIP ) Sotherend=bm.nop.back();
  if ( bm.iclip>0  && bm.op[0]==BAM_CSOFT_CLIP ) Sotherend=bm.nop[0];
  //if ( Sotherend > SEQ.size()*0.08 ) return false;
  
  iread.tid=b->core.tid;
  iread.pos=bm.pos;
  iread.q1=b->core.qual;
  iread.len=bm.l_qseq;
  iread.M=bm.nop[bm.anchor];
  iread.Mrpos=bm.qop[bm.anchor];
  iread.S=bm.nop[bm.iclip];
  iread.sbeg=bm.cop[bm.iclip];
  iread.send=bm.cop[bm.iclip]+bm.nop[bm.iclip]-1;
  iread.mms=ndiff_s;
  iread.mmm=ndiff_m;
  
  return true;
}

//! find displacement that the break points can slide by the same step
//! return: dx_F2 displacement in positive direction
//! return: dx_R1 displacement in negative direction
//! both dx_F2 and dx_R1 are non negative numbers
//! total displace is dx_F2+dx_R1
//! left most start is F2-dx_R1
void find_displacement(string& FASTA, int F2, int R1, 
		       int& dx_F2, int& dx_R1)
{
  dx_F2=0;
  dx_R1=0;
  if ( F2>=(int)FASTA.size() || R1>=(int)FASTA.size() ) return;
  if ( F2<=0 || R1<=0 ) return;
  
  bool after=true;
  while( after ) {
    if ( F2+dx_F2+1>=(int)FASTA.size() || 
	 R1+dx_F2>=(int)FASTA.size() ) break;
    if ( FASTA[F2+dx_F2+1] != FASTA[R1+dx_F2] ) after=false;
    dx_F2+=(int)after;
  }
  
  bool prev=true;
  while( prev ) {
    if ( F2-dx_R1<=0 || R1-dx_R1-1<=0 ) break;
    if ( FASTA[F2-dx_R1] != FASTA[R1-dx_R1-1] ) prev=false;
    dx_R1+=(int)prev;
  }
  
}

void check_map_quality(int ref, int beg, int end, double& q0, double& q1) 
{
  q0=q1=0;
  if ( beg>end ) swap(beg, end);
  if ( ref<0 || ref>=msc::fp_in->header->n_targets ) {
    cerr << "warning: ref out of range, ref=" << ref << endl;
    exit(0);
    return;
  }
  
  if ( abs(beg-end)>1E6 ) { q0=q1=-0.01001; return; }
  
  bam1_t *b=NULL; b = bam_init1();
  bam_iter_t iter=0;
  
  double d0=0, d1=0;
  double count=0;
  iter = bam_iter_query(msc::bamidx, ref, beg, end);
  while( bam_iter_read(msc::fp_in->x.bam, iter, b)>0 ) {
    count+=1;
    if ( b->core.qual==0 ) d0+=1;
    if ( b->core.qual<=10 ) d1+=1;
    if ( b->core.tid!=ref || b->core.pos>end ) break;
  }
  if ( b ) bam_destroy1(b);
  if ( iter ) bam_iter_destroy(iter);
  
  q0=q1=-0.01001;
  if ( count>1 ) {
    q0=d0/count;
    q1=d1/count;
  }
  
  return;
}

int mean_readdepth(int ref, int beg, int end) 
{
  if ( beg>end ) swap(beg, end);
  if ( ref<0 || ref>=msc::fp_in->header->n_targets ) {
    cerr << "warning: ref out of range, ref=" << ref << endl;
    exit(0);
    return(-1);
  }
  
  bam1_t *b=NULL; b = bam_init1();
  bam_iter_t iter=0;
  
  double dx=end-beg+1;
  double d1=0;
  double count=0;
  iter = bam_iter_query(msc::bamidx, ref, beg, end);
  while( bam_iter_read(msc::fp_in->x.bam, iter, b)>0 ) {
    if ( ! is_read_count_for_depth(b) ) continue;
    count++;
    
    POSCIGAR_st b_m;
    resolve_cigar_pos(b, b_m, 0);
    for(size_t i=0; i<b_m.nop.size(); ++i) {
      int op=b_m.op[i];
      if ( op == BAM_CMATCH || 
	   op == BAM_CEQUAL || 
	   op == BAM_CDIFF ) {
	int r_beg=b_m.cop[i];
	int r_end=b_m.cop[i]+b_m.nop[i]-1;
	int r1=max(beg, r_beg);
	int r2=min(end, r_end);
	if ( r2>=r1 ) d1+=r2-r1+1;
      }
    }
    
  }
  d1/=dx;
  //cerr << "reads:\t" << ref << "\t" << beg << "-" << end << "\t" << count << endl;
  
  if ( b ) bam_destroy1(b);
  if ( iter ) bam_iter_destroy(iter);

  return(d1);
}

int median_readdepth(int ref, int beg, int end) 
{
  if ( beg>end ) swap(beg, end);
  if ( ref<0 || ref>=msc::fp_in->header->n_targets ) {
    cerr << "warning: ref out of range, ref=" << ref << endl;
    exit(0);
    return(-1);
  }
  
  bam1_t *b=NULL; b = bam_init1();
  bam_iter_t iter=0;
  
  int dx=end-beg+1;
  vector<int> rd(dx,0);
  iter = bam_iter_query(msc::bamidx, ref, beg, end);
  while( bam_iter_read(msc::fp_in->x.bam, iter, b)>0 ) {
    if ( ! is_read_count_for_depth(b) ) continue;
    POSCIGAR_st b_m;
    resolve_cigar_pos(b, b_m, 0);
    for(size_t i=0; i<b_m.nop.size(); ++i) {
      int op=b_m.op[i];
      if ( op == BAM_CMATCH || 
	   op == BAM_CEQUAL || 
	   op == BAM_CDIFF ) {
	int r_beg=b_m.cop[i];
	int r_end=b_m.cop[i]+b_m.nop[i]-1;
	int r1=max(beg, r_beg);
	int r2=min(end, r_end);
	if ( r2>=r1 ) for(int k=r1; k<=r2; ++k) rd[k-beg]+=1;
      }
    }
  }
  
  size_t midIndex = rd.size()/2;
  std::nth_element(rd.begin(), rd.begin() + midIndex, rd.end());  
  
  return( rd[midIndex] );
}

void check_cnv_readdepth_from_bam(int ref, int beg, int end, int dx, 
			 int& d1, int& d2, int& din) 
{
  d1=din=d2=-1;
  if ( beg>end ) swap(beg, end);
  if ( ref<0 || ref>=msc::fp_in->header->n_targets ) {
    cerr << "check_cnv_readdepth warning: ref out of range, ref=" << ref << endl;
    exit(0);
  }
  
  if ( abs(end-beg)>1000000 ) {
    din=-1;
    d1=mean_readdepth(msc::bam_ref, max(1, beg-dx),beg-1);
    d2=mean_readdepth(msc::bam_ref, end+1, end+dx);
    return;
  }
  
  bam1_t *b=NULL; b = bam_init1();
  bam_iter_t iter=0;
  
  double rd1=0, rd2=0, rdin=0;
  
  iter = bam_iter_query(msc::bamidx, ref, max(0, beg-dx), end+dx);
  while( bam_iter_read(msc::fp_in->x.bam, iter, b)>0 ) {
    if ( ! is_read_count_for_depth(b) ) continue;
    
    POSCIGAR_st b_m;
    resolve_cigar_pos(b, b_m, 0);
    for(size_t i=0; i<b_m.nop.size(); ++i) {
      int op=b_m.op[i];
      if ( op == BAM_CMATCH || 
	   op == BAM_CEQUAL || 
	   op == BAM_CDIFF ) {
	int r_beg=b_m.cop[i];
	int r_end=b_m.cop[i]+b_m.nop[i]-1;
	
	int r1, r2;
	
	r1=max(beg-dx, r_beg);
	r2=min(beg-1, r_end);
	if ( r2>=r1 ) rd1+=r2-r1+1;
	
	r1=max(end+1, r_beg);
	r2=min(end+dx, r_end);
	if ( r2>=r1 ) rd2+=r2-r1+1;
	
	r1=max(beg, r_beg);
	r2=min(end, r_end);
	if ( r2>=r1 ) rdin+=r2-r1+1;

      }
    }
    
  }
  if ( b ) bam_destroy1(b);
  if ( iter ) bam_iter_destroy(iter);
  
  rd1/=dx;
  rd2/=dx;
  rdin/=(double)(end-beg+1);
  
  d1=rd1+0.5;
  d2=rd2+0.5;
  din=rdin+0.5;

  return;
}

void check_cnv_readdepth(int ref, int beg, int end, int dx, 
			 int& d1, int& d2, int& din) 
{
  d1=din=d2=-1;
  bool switched=false;
  if ( beg>end ) {
    swap(beg, end);
    switched=true;
  }
  
  if ( ref != msc::bam_ref || 
       msc::rd.size() != msc::fp_in->header->target_len[ref] ) {
    cerr << "this subsroutine is not intended for tid:" << ref 
	 << " of length " << msc::fp_in->header->target_len[ref] << endl
	 << "current buffer is for tid:" << msc::bam_ref
	 << " of length " << msc::rd.size() << endl;
    exit(0);
  }
  
  double rd1=0, rd2=0, rdin=0;
  for(int i=beg-dx+1-switched; i<=beg-switched; ++i) 
    if ( i>=0 && i<(int)msc::rd.size() ) rd1 += msc::rd[i] ;  
  for(int i=end+switched; i<end+dx+switched; ++i) 
    if ( i>=0 && i<(int)msc::rd.size() ) rd2 += msc::rd[i] ;  
  for(int i=beg+1-switched; i<end+switched; ++i) 
    if ( i>=0 && i<(int)msc::rd.size() ) rdin += msc::rd[i] ;  
  
  rd1/=(double)dx;
  rd2/=(double)dx;
  rdin/=(double)(end-beg-1+switched+switched+0.000000001f);
  
  d1=rd1+0.5;
  d2=rd2+0.5;
  din=rdin+0.5;
  if ( switched ) swap(d1, d2);
  
  return;
}

void check_cnv_readdepth_100(int ref, int beg, int end, 
			     int& d1, int& din1, int& din2, int& d2) 
{
  d1=d2=din1=din2=-1;

  int dx=100;
  bool switched=false;
  if ( beg>end ) {
    swap(beg, end);
    switched=true;
  }
  
  if ( ref != msc::bam_ref || 
       msc::rd.size() != msc::fp_in->header->target_len[ref] ) {
    cerr << "this subsroutine is not intended for tid:" << ref 
	 << " of length " << msc::fp_in->header->target_len[ref] << endl
	 << "current buffer is for tid:" << msc::bam_ref
	 << " of length " << msc::rd.size() << endl;
    exit(0);
  }
  
  double rd1=0, rd2=0, rdin1=0, rdin2=0;
  for(int i=beg-dx+1-switched; i<=beg-switched; ++i) 
    if ( i>=0 && i<(int)msc::rd.size() ) rd1 += msc::rd[i] ;  
  rd1/=(double)dx;
  d1=rd1+0.5;
  
  for(int i=end+switched; i<end+dx+switched; ++i) 
    if ( i>=0 && i<(int)msc::rd.size() ) rd2 += msc::rd[i] ;  
  rd2/=(double)dx;
  d2=rd2+0.5;
  
  if ( end-beg<=dx ) {
    rdin1=0;
    for(int i=beg+1-switched; i<end+switched; ++i) 
      if ( i>=0 && i<(int)msc::rd.size() ) rdin1 += msc::rd[i] ;  
    rdin1/=(double)(end-beg-1+switched+switched+0.000000001f);
    din1=rdin1+0.5;
    din2=din1;
  }
  else {
    rdin1=0;
    for(int i=beg+1-switched; i<=beg+1-switched+dx; ++i) 
      if ( i>=0 && i<(int)msc::rd.size() ) rdin1 += msc::rd[i] ;  
    rdin1/=(double)(dx+0.000000001f);
    din1=rdin1+0.5;
    
    rdin2=0;
    for(int i=end+switched-dx; i<end+switched; ++i) 
      if ( i>=0 && i<(int)msc::rd.size() ) rdin2 += msc::rd[i] ;  
    rdin2/=(double)(dx+0.000000001f);
    din2=rdin2+0.5;
  }
  
  
  if ( switched ) { 
    swap(d1, d2); 
    swap(din1, din2);
  }
  
  return;
}

//! check if readMS and readSM overlap with miinimum minOver charactors
//! maxErr mismatches are tolorated
//! readMS is always fully overlaped with or in front of readSM
//! p1 is the position on readMS where overlap begins
//! p1=-1 if not overlaped
//! p_err stores indices mismatched charactors in readMS
bool string_overlap(const string& readMS, const string& readSM, 
		    const int minOver, const int maxErr,
		    int& p1, vector<int>& p_err) 
{
  bool match=false;
  p1=-1;
  p_err.clear();
  
  int i,j,k, ndiff, p1_opt=-1;
  //int emin=readMS.length()+readSM.length();
  double err_rate=1.0;
  
  //  for(i=0; i<(int)readMS.length()-minOver; ++i) {
  // match from end to beg and stop at first match and forward 10 more
  for(i=readMS.length()-minOver-1; i>=0; --i) {
    if ( match && i<p1_opt-10 ) break;
    ndiff=0;
    for(k=i,j=0; k<(int)readMS.length() && j<(int)readSM.length(); ++k,++j) {
      ndiff += ( readMS[k]!=readSM[j] );
      if (ndiff>maxErr) break;
    }
    if (ndiff<=maxErr) { 
      match=true;
      p1=i;
      // if ( ndiff<emin ) { p1_opt=p1; emin=ndiff; } 
      if ( (double)ndiff/(double)(j+1) < err_rate ) { 
	p1_opt=p1; 
	err_rate=(double)ndiff/(double)(j+1);
      } 
      if ( ndiff==0 ) break;
      // break;
    }
  }
  
  if ( match ) {
    p1=p1_opt;
    ndiff=0;
    for(k=p1,j=0; k<(int)readMS.length() && j<(int)readSM.length(); ++k,++j) 
      if ( readMS[k]!=readSM[j] ) p_err.push_back( k );
    if ( (int)p_err.size() > maxErr ) {
      cerr << "string_overlap(): Error matching" << endl;
      exit(0);
    }
  }
  
  return(match);
}

bool string_overlap_front(const string& readMS, const string& readSM, 
		    const int minOver, const int maxErr,
		    int& p1, vector<int>& p_err) 
{
  bool match=false;
  p1=-1;
  p_err.clear();
  
  int i,j,k, ndiff, p1_opt=-1;
  //int emin=readMS.length()+readSM.length();
  double err_rate=1.0;
  
  for(i=0; i<(int)readMS.length()-minOver; ++i) {
    ndiff=0;
    for(k=i,j=0; k<(int)readMS.length() && j<(int)readSM.length(); ++k,++j) {
      ndiff += ( readMS[k]!=readSM[j] );
      if (ndiff>maxErr) break;
    }
    if (ndiff<=maxErr) { 
      match=true;
      p1=i;
      // if ( ndiff<emin ) { p1_opt=p1; emin=ndiff; } 
      if ( (double)ndiff/(double)(j+1) < err_rate ) { 
	p1_opt=p1; 
	err_rate=(double)ndiff/(double)(j+1);
      } 
      if ( ndiff==0 ) break;
      // break;
    }
  }
  
  if ( match ) {
    p1=p1_opt;
    ndiff=0;
    for(k=p1,j=0; k<(int)readMS.length() && j<(int)readSM.length(); ++k,++j) 
      if ( readMS[k]!=readSM[j] ) p_err.push_back( k );
    if ( (int)p_err.size() > maxErr ) {
      cerr << "string_overlap(): Error matching" << endl;
      exit(0);
    }
  }
  
  return(match);
}


/*! read bF2 and bR1 overlap starting from p1(0 based) of bF2, find the break points
  | the break points if exist, must be between p1-1 to end of bF2 on read bF2
  | the best candidates are those that the points on both are M
  | the second choice would be those with S on bF2 and M on bR1
  | the next choice would be those with M on bF2 and S on bR1
  | if the above are not availble, just use p1-1
  | given a list of possible points, test the first and the last
  | ideally the CIGAR score should be calibrated first
*/
void get_break_points(const string& FASTA, bam1_t *bF2, bam1_t *bR1, int p1, vector<int>& p_err, 
		      int& F2, int& R1, int& e_dis)
{
  F2=R1=-1;
  e_dis=1000000;
  if ( bF2->core.flag & BAM_FUNMAP || bF2->core.n_cigar==0 || bF2->core.pos<0 ) return;
  if ( bF2->core.flag & BAM_FUNMAP || bR1->core.n_cigar==0 || bR1->core.pos<0 ) return;
  
  // 0 based positions on FASTA
  POSCIGAR_st bF2_m, bR1_m;
  resolve_cigar_pos(bF2, bF2_m, 0);
  resolve_cigar_pos(bR1, bR1_m, 0);
  
  // basic filter; return false if too close to REF ends
  // totally overlap
  if ( p1==0 ) p1=1;
  int bpF2=get_pos_for_base(bF2_m, p1-1);
  int bpR1=get_pos_for_base(bR1_m, 0);
  
  if ( bpF2<1 || bpF2+bF2->core.l_qseq>(int)FASTA.size() ) return;
  if ( bpR1<1 || bpR1+bR1->core.l_qseq>(int)FASTA.size() ) return;
  
  // find break points using projected referene to approximate ED
  string F_qseq=get_qseq(bF2);
  string F_qseq_ref=ref_projected_onto_qseq(bF2, FASTA);
  string R_qseq=get_qseq(bR1);
  string R_qseq_ref=ref_projected_onto_qseq(bR1, FASTA);
  
  string concatinated_read=F_qseq.substr(0,p1)+R_qseq;
  string projected_ref=F_qseq_ref.substr(0,p1)+R_qseq_ref;
  
  vector<int> ED0(bF2->core.l_qseq, 1000000); 
  int br_beg=p1-1;
  int ndiff_F2R1=0;
  for(size_t i=0; i<concatinated_read.size(); ++i) 
    ndiff_F2R1 += ( projected_ref[i] != concatinated_read[i] ) ;
  ED0[br_beg]=ndiff_F2R1;
  int ndiff_imin=br_beg;
  int ndiff_min=ndiff_F2R1;
  int br_stop=min( (int)bF2->core.l_qseq, (int)concatinated_read.size() )-1;
  for( br_beg=p1; br_beg<br_stop; ++br_beg ) {
    char br_base_qseq = F_qseq[br_beg];
    char br_base_ref  = F_qseq_ref[br_beg];
    
    ndiff_F2R1 += ( br_base_qseq != br_base_ref )
      - ( projected_ref[br_beg] != concatinated_read[br_beg] ) ;
    
    concatinated_read[br_beg] = br_base_qseq;
    projected_ref[br_beg] = br_base_ref;
    
    ED0[br_beg]=ndiff_F2R1;
    if ( ndiff_F2R1 < ndiff_min ) {
      ndiff_min = ndiff_F2R1 ;
      ndiff_imin = br_beg;
    }
  }
  
  // get possible break points based on CIGAR
  vector<int> F2_e_cigar(0), R1_e_cigar(0); 
  expand_cigar(bF2, F2_e_cigar);
  expand_cigar(bR1, R1_e_cigar);
  // possible good break points
  vector<int> F2_br(0);
  // in ideal condition, break points should fall on M parts
  string bptype="MM";
  for(int q=p1-1, r=0; q<bF2->core.l_qseq && r<bR1->core.l_qseq; ++q,++r) {
    if ( ( F2_e_cigar[q]==BAM_CMATCH || F2_e_cigar[q]==BAM_CEQUAL ) &&
	 ( R1_e_cigar[r]==BAM_CMATCH || R1_e_cigar[r]==BAM_CEQUAL ) ) {
      F2_br.push_back(q);
    }
  }
  // if not any, find break points based on R1's  M parts
  if ( F2_br.size()==0 ) {
    bptype="SM";
    for(int q=p1-1, r=0; q<bF2->core.l_qseq && r<bR1->core.l_qseq; ++q,++r) {
      if ( ( R1_e_cigar[r]==BAM_CMATCH || R1_e_cigar[r]==BAM_CEQUAL ) ) {
	F2_br.push_back(q);
      }
    }
  }
  // if still not any, find break points based on F2's M parts
  if ( F2_br.size()==0 ) {
    bptype="MS";
    for(int q=p1-1, r=0; q<bF2->core.l_qseq && r<bR1->core.l_qseq; ++q,++r) {
      if ( ( F2_e_cigar[q]==BAM_CMATCH || F2_e_cigar[q]==BAM_CEQUAL ) ) {
	F2_br.push_back(q);
      }
    }
  }
  // if still not any, just the 1st and the middle
  if ( F2_br.size()==0 ) {
    bptype="SS";
    F2_br.push_back(p1-1);
    F2_br.push_back( (p1+min(p1+bR1->core.l_qseq,bF2->core.l_qseq))/2 );
  }
  // from minimum ED positions, get the first MM
  for( br_beg=p1-1; br_beg<bF2->core.l_qseq; ++br_beg ) {
    if ( ED0[br_beg] > ndiff_min ) continue;
    std::vector<int>::iterator it;
    it=find (F2_br.begin(), F2_br.end(), br_beg);
    if ( it != F2_br.end() ) { 
      ndiff_imin=br_beg;
      break;
    }
  }
  
  // whether or not the above code is useful, ndiff_imin is already calculated
  F2=get_pos_for_base(bF2_m, ndiff_imin);
  R1=get_pos_for_base(bR1_m, ndiff_imin-p1+1);
  if ( F2 < 0 ) cerr << get_cigar(bF2) << "\t" << ndiff_imin << endl;
  if ( R1 < 0 ) cerr << get_cigar(bR1) << "\t" << ndiff_imin << "\t" << p1+1 << endl;
  e_dis=ndiff_min;
  if ( msc::verbose>1 ) {
    cerr << "edit_distance  " 
	 << bF2->core.pos << " " << get_cigar(bF2) << "\t"
	 << bR1->core.pos << " " << get_cigar(bR1) << "\t"
	 << e_dis << "\t" << p1 << "\t" << ndiff_imin << "\t" 
	 << F2 << "\t" << R1
	 << endl;
  }
  /*
  // find break points using edit_distance
  int F_beg=get_pos_for_base(bF2_m, 0);
  int R_end=get_pos_for_base(bR1_m, bR1->core.l_qseq-1);
  string concatinated_ref=
    FASTA.substr(F_beg, bpF2-F_beg+1)+
    FASTA.substr(bpR1, R_end-bpR1+1);
  
  vector<int> ED( F2_br.size(), 1000000 );
  for(int i=0; i<(int)F2_br.size(); ++i) {
    // it should be enough just to check the first and last location
    if ( i!=0 && i!=(int)F2_br.size()-1 ) continue;
    int q=F2_br[i];
    int r=q-p1+1;
    for(int k=0; k<(int)p_err.size(); ++k) {
      if ( p_err[k]<= q ) concatinated_read[p_err[k]]=F_qseq[p_err[k]];
    }
    bpF2=get_pos_for_base(bF2_m, q);
    bpR1=get_pos_for_base(bR1_m, r);
    concatinated_ref=
      FASTA.substr(F_beg, bpF2-F_beg+1)+
      FASTA.substr(bpR1, R_end-bpR1+1);
    ED[i]=edit_distance(concatinated_read, concatinated_ref);
  }
  int ir=0;
  for(int i=0; i<(int)ED.size(); ++i) if ( ED[i]<ED[ir] ) ir=i;
  
  if ( msc::verbose >1 ) 
    cerr << "edit_distance\t" 
	 << ndiff_min << "\t" << ndiff_imin << "\t" 
	 << e_dis << "\t" << F2_br[ir] << endl;
  
  int p_min= ED[ir] < ndiff_min ? F2_br[ir] : ndiff_imin ;
  F2=get_pos_for_base(bF2_m, p_min);
  R1=get_pos_for_base(bR1_m, p_min-p1+1);
  e_dis=min( ED[ir], ndiff_min );
  */
  /*
  if ( e_dis>p_err.size() ) {
    cerr << "matcherr\t" << bF2->core.pos << "\t" << get_cigar(bF2) << "\t"
	 << bR1->core.pos << "\t" << get_cigar(bR1) << "\t"
	 << e_dis << "\t" << p1 << "\t" << p_min << endl;
  }
  */

  return;
}

void prepare_pairend_matchclip_data(int ref, int beg, int end, 
				    int min_pair_length,
				    string& FASTA,
				    vector<intpair_st>& pairs,
				    vector<bam1_t>& b_MS, vector<bam1_t>& b_SM) 
{
  pairs.clear();
  msc::bdata.clear();
  msc::bdata.reserve(300000000);
  b_MS.clear();
  b_SM.clear();
  
  msc::rd.reserve(FASTA.size()+100);
  msc::rd.resize(FASTA.size(),0);

  vector<size_t> p_MS(0),p_SM(0);
  
  bam1_t *b=NULL; b = bam_init1();
  bam_iter_t iter=0;
  
  bam1_t ibam;
  intpair_st ipair;
  
  double isize=0.0, isize2=0.0, isize_c=0, isize_sd=0.0;
  
  iter = bam_iter_query(msc::bamidx, ref, beg, end);
  size_t count=0;
  int bam_beg=0, bam_end=0;
  while( bam_iter_read(msc::fp_in->x.bam, iter, b)>0 ) {
    if ( b->core.tid != msc::bam_ref ) break;
    if ( b->core.pos > end ) break;
    if ( count==0 ) bam_beg=b->core.pos;
    bam_end=b->core.pos;
    count++;
    if ( count%1000000==0 ) {
      cerr << "#processed " << commify(count) << " reads at pos " 
	   << string(msc::fp_in->header->target_name[ref]) 
	   << "@" << commify(b->core.pos) 
	   << endl;
    }
    
    if (  is_read_count_for_depth(b, 0) ) {
      // get read depth
      POSCIGAR_st b_m;
      resolve_cigar_pos(b, b_m, 0);
      for(size_t i=0; i<b_m.nop.size(); ++i) {
	int op=b_m.op[i];
	if ( op == BAM_CMATCH || 
	     op == BAM_CEQUAL || 
	     op == BAM_CDIFF ) {
	  int r_beg=b_m.cop[i];
	  int r_end=b_m.cop[i]+b_m.nop[i]-1;
	  if ( r_beg>=0 && r_end<(int)FASTA.size() ) 
	    for(int k=r_beg; k<r_end; ++k) msc::rd[k]+=1;
	}
      }
    }
    
    if ( b->core.mpos >= beg && b->core.mpos <= end &&
	 abs(b->core.isize) >= min_pair_length && 
	 is_read_count_for_pair(b) ) {
      check_inner_pair_ends(b, ipair.F2, ipair.F2_acurate, ipair.R1, ipair.R1_acurate);
      if ( ipair.F2>0 && ipair.R1>0 ) pairs.push_back(ipair);
    }
    
    // calculate insert and sd again
    if ( (b->core.flag & BAM_FPROPER_PAIR) &&
	 !(b->core.flag & BAM_DEF_MASK) &&
	 abs(b->core.isize) > msc::bam_pe_insert-10*msc::bam_pe_insert_sd &&
	 abs(b->core.isize) < msc::bam_pe_insert+10*msc::bam_pe_insert_sd ) {
      isize   += abs(b->core.isize);
      isize2  += (double)b->core.isize * (double)b->core.isize;
      isize_c += 1;
    }
    
    RSAI_st iread;
    if (  is_keep_read(b, FASTA, iread) ) {
      // save read in buffer
      size_t p_pos=msc::bdata.size();
      _save_read_in_vector(b, ibam, msc::bdata);
      if ( iread.sbeg > iread.pos ) {
	// type M...S    
	b_MS.push_back(ibam);  // save bam_t
	p_MS.push_back(p_pos); // save relative pointer to vector
      }
      else {
	// type S...M
	b_SM.push_back(ibam);  // save bam_t
	p_SM.push_back(p_pos); // save relative pointer to vector
      }
    }
  }
  bam_destroy1(b);
  bam_iter_destroy(iter);
  
  // the reason to take the trouble is because address of vector may change due to
  // change of size
  for(size_t i=0; i<b_MS.size(); ++i) b_MS[i].data = &msc::bdata[ (size_t)b_MS[i].data ];
  for(size_t i=0; i<b_SM.size(); ++i) b_SM[i].data = &msc::bdata[ (size_t)b_SM[i].data ];
  
  vector<bool> tokeep( pairs.size(), true );
  for(size_t i=0; i<pairs.size(); ++i) {
    if ( pairs[i].F2<bam_beg || pairs[i].F2>bam_end ||
	 pairs[i].R1<bam_beg || pairs[i].R1>bam_end ) tokeep[i]=false;
  }
  
  if ( isize_c>2 ) {
    isize/=isize_c;
    isize_sd =sqrt( (isize2-isize*isize*isize_c)/isize_c );
    if ( isize_sd<30 ) {
      cerr << "isize sd is too small " << isize_sd << " changed to 50" << endl;  
      isize_sd=50;
    }
    if ( !msc::bam_pe_set_by_user ) {    
      msc::bam_is_paired=true;
      msc::bam_pe_insert=(int)isize;
      msc::bam_pe_insert_sd=(int)isize_sd;
    }
  }
  
  size_t k=0;
  for(size_t i=0; i<pairs.size(); ++i) if ( tokeep[i] ) { pairs[k]=pairs[i]; ++k;}
  if ( k<pairs.size() ) pairs.erase( pairs.begin()+k, pairs.end() );
  
  cerr << "data range " << string(msc::fp_in->header->target_name[ref]) 
       << ":" << commify(bam_beg) << "-" << commify(bam_end) << "\n"
       << "MS:" << b_MS.size() << "  SM:" << b_SM.size() << "  CIGAR_SEQ:" << msc::bdata.size() 
       << "  AbnormalPairs:" << pairs.size() << "\n"
       << "Pair insert:" << (int)isize << " += " << (int)isize_sd << "\n"
       << "memory used by reads\t" 
       << commify(totalRAM(b_MS)+totalRAM(b_SM)+totalRAM(msc::bdata)) << "\n"
       << "memory used by pairs\t" 
       << commify( totalRAM(pairs) ) << "\n"
       << "memory used by read depth\t" 
       << commify( totalRAM(msc::rd) ) 
       << endl;  
  
  return;
}

void stat_region(pairinfo_st& ibp) 
{
  
  int dx = msc::bam_l_qseq*5;
  if ( dx/3 > abs(ibp.F2-ibp.R1) ) dx=abs(ibp.F2-ibp.R1)*3;
  if ( dx < msc::bam_l_qseq ) dx = msc::bam_l_qseq;
  
  // check read depth information
  int ref=ibp.tid;
  int F2=ibp.F2;
  int R1=ibp.R1;
  if ( F2>R1 ) swap(F2, R1);
  if ( R1-F2>msc::bam_l_qseq/2 ) {
    int oldminq=msc::minMAPQ;
    msc::minMAPQ=0;
    ibp.F2_rd=mean_readdepth(ref, max(1, F2-dx), F2);
    ibp.R1_rd=mean_readdepth(ref, R1, R1+dx);
    ibp.rd=mean_readdepth(ref, F2, R1);
    msc::minMAPQ=oldminq;
  }
  
  // check pair end information
  if ( msc::bam_is_paired && 
       abs(ibp.R1-ibp.F2)>msc::bam_pe_insert_sd*3 ) {
    check_normal_and_abnormalpairs_cross_region(ibp.tid, ibp.F2, ibp.R1,
						ibp.F2_rp, ibp.R1_rp, 
						ibp.FRrp);      
  }
  
  return;
}

void stat_region(pairinfo_st& ibp, string& FASTA, int dx) 
{
  
  if ( ibp.tid == msc::bam_ref &&
       FASTA.size() == msc::fp_in->header->target_len[ibp.tid] ) {
    int dx_F2=0, dx_R1=0;
    find_displacement(FASTA, ibp.F2, ibp.R1, dx_F2, dx_R1);
    ibp.un=dx_F2+dx_R1;
  }
  
  if ( dx<=0 ) {
    dx=abs(ibp.F2-ibp.R1)*2;
    if ( dx>msc::bam_l_qseq*5 ) dx=msc::bam_l_qseq*5;
    if ( dx<msc::bam_l_qseq ) dx=msc::bam_l_qseq;
  }
  
  check_cnv_readdepth(ibp.tid, ibp.F2, ibp.R1, dx, 
		      ibp.F2_rd, ibp.R1_rd, ibp.rd);
  
  check_cnv_readdepth_100(ibp.tid, ibp.F2, ibp.R1, 
			  ibp.F2_rd_100, ibp.rd_F2_100, 
			  ibp.rd_R1_100, ibp.R1_rd_100);
  
  if ( msc::bam_is_paired && 
       ( ibp.R1-ibp.F2>msc::bam_pe_insert_sd*3 || 
	 ibp.R1-ibp.F2<-msc::bam_l_qseq ) ) {
    check_normal_and_abnormalpairs_cross_region(ibp.tid, ibp.F2, ibp.R1,
						ibp.F2_rp, ibp.R1_rp, 
						ibp.FRrp);      
  }
  
  return;
}

void assess_rd_rp_sr_infomation(pairinfo_st& ibp, int medRD, int medRP) 
{

  // 1. /2 for one chromosome, 
  // 2. /2 for variation, 
  // 3. /2 for mapping error
  // 4. /2 for mapq=0 reads
  int minRP=medRP/16;
  if ( minRP<5 ) minRP=5;
  
  int rpscore=0;
  int rdscore=0;
  int ddscore=0;
  int srscore=0;
  
  // find out normal read depth and pairs around bp
  // when rd_normal is too low, it is unreliable anyway
  // int rd_normal=(ibp.F2_rd + ibp.R1_rd)/2;
  //int rd_expected=(double)rd_normal/2.0f*msc::bam_pe_insert/2.0f/msc::bam_l_qseq;
  
  //int pr_normal=max( 20, min(ibp.F2_rp, ibp.R1_rp) );
  //  pr_normal = max( pr_normal, max(ibp.F2_rp, ibp.R1_rp)/3 );
  //  pr_normal = max( pr_normal, medRP/2 );
  //  pr_normal = min( pr_normal, max(ibp.F2_rp, ibp.R1_rp) );
  //  pr_normal = max( pr_normal, 20 );
  
  // expected pairs for variation
  int pr_normal=max( 20, max(ibp.F2_rp, ibp.R1_rp) );
  int pr_expected= ibp.F2 > ibp.R1 ? pr_normal/2 : pr_normal;
  // read pair score
  if ( ibp.FRrp>pr_expected*1/3 ) rpscore=1;
  if ( ibp.FRrp>pr_expected*1/2 ) rpscore=2;
  if ( ibp.FRrp>pr_expected*2/3 ) rpscore=3;
  if ( ibp.FRrp>pr_expected*3/4 ) rpscore=4;
  if ( ibp.FRrp<=4 ) rpscore=0; // low signal override
  
  // read depth score
  if ( ibp.F2 < ibp.R1 ) { // DEL type of signal
    int rd_normal=min(ibp.F2_rd, ibp.R1_rd);
    if ( ibp.rd>=0 && ibp.rd < rd_normal*3/4 ) rdscore=1;
    if ( ibp.rd>=0 && ibp.rd < rd_normal*2/3 ) rdscore=2;
    if ( ibp.rd>=0 && ibp.rd < rd_normal*5/9 ) rdscore=3;
    if ( ibp.rd>=0 && ibp.rd < rd_normal*1/5 ) rdscore=4;
  }
  else { // DUP type
    int rd_normal=max(ibp.F2_rd, ibp.R1_rd);
    if ( ibp.rd>=0 && ibp.rd > rd_normal*5/4 ) rdscore=1;
    if ( ibp.rd>=0 && ibp.rd > rd_normal*4/3 ) rdscore=2;
    if ( ibp.rd>=0 && ibp.rd > rd_normal*3/2 ) rdscore=3;
    if ( ibp.rd>=0 && ibp.rd > rd_normal*2 ) rdscore=4;
  }
  if ( min(ibp.F2_rd, ibp.R1_rd) < 8 ) rdscore=0; // low signal override
  
  // derivative of read depth score
  if ( ibp.F2 < ibp.R1 ) { 
    // DEL type of signal
    if ( ibp.rd_F2_100 < ibp.F2_rd_100*3/4 && 
	 ibp.rd_R1_100 < ibp.R1_rd_100*3/4 ) ddscore=1;
    if ( ibp.rd_F2_100 < ibp.F2_rd_100*2/3 && 
	 ibp.rd_R1_100 < ibp.R1_rd_100*2/3 ) ddscore=2;
    if ( ibp.rd_F2_100 < ibp.F2_rd_100*4/7 && 
	 ibp.rd_R1_100 < ibp.R1_rd_100*4/7 ) ddscore=3;
    if ( ibp.rd_F2_100 < ibp.F2_rd_100*1/2 && 
	 ibp.rd_R1_100 < ibp.R1_rd_100*1/2 ) ddscore=4;
  }
  else { 
    // DUP type
    if ( ibp.rd_F2_100 > ibp.F2_rd_100*5/4 && 
	 ibp.rd_R1_100 > ibp.R1_rd_100*5/4 ) ddscore=1;
    if ( ibp.rd_F2_100 > ibp.F2_rd_100*4/3 && 
	 ibp.rd_R1_100 > ibp.R1_rd_100*4/3 ) ddscore=2;
    if ( ibp.rd_F2_100 > ibp.F2_rd_100*3/2 && 
	 ibp.rd_R1_100 > ibp.R1_rd_100*3/2 ) ddscore=3;
    if ( ibp.rd_F2_100 > ibp.F2_rd_100*9/5 && 
	 ibp.rd_R1_100 > ibp.R1_rd_100*9/5 ) ddscore=4;
  }
  if ( min(ibp.F2_rd_100, ibp.R1_rd_100) < 8 ) ddscore=0;
  
  
  // matching read score
  if ( ibp.F2_rd>0 && ibp.R1_rd>0 ) { 
    // read depth available
    if ( ibp.F2_sr*8 > ibp.F2_rd || ibp.R1_sr*8 > ibp.R1_rd ) srscore=1;
    if ( (ibp.F2_sr*4 > ibp.F2_rd || ibp.R1_sr*4 > ibp.R1_rd) &&
	 (ibp.F2_sr*8 > ibp.F2_rd && ibp.R1_sr*8 > ibp.R1_rd) ) srscore=2;
    if ( (ibp.F2_sr*3 > ibp.F2_rd ||  ibp.R1_sr*3 > ibp.R1_rd) &&
	 (ibp.F2_sr*4 > ibp.F2_rd && ibp.R1_sr*4 > ibp.R1_rd) ) srscore=3;
    if ( ibp.F2_sr*3 > ibp.F2_rd &&  ibp.R1_sr*3 > ibp.R1_rd ) srscore=4;
  }
  else {
    // read depth at bp points are always available
    if ( ibp.F2_sr*8 > ibp.MS_F2_rd || ibp.R1_sr*8 > ibp.MS_R1_rd ) srscore=1;
    if ( (ibp.F2_sr*4 > ibp.MS_F2_rd || ibp.R1_sr*4 > ibp.MS_R1_rd) &&
	 (ibp.F2_sr*8 > ibp.MS_F2_rd && ibp.R1_sr*8 > ibp.MS_R1_rd) ) srscore=2;
    if ( (ibp.F2_sr*3 > ibp.MS_F2_rd ||  ibp.R1_sr*3 > ibp.MS_R1_rd) &&
	 (ibp.F2_sr*4 > ibp.MS_F2_rd && ibp.R1_sr*4 > ibp.MS_R1_rd) ) srscore=3;
    if ( ibp.F2_sr*3 > ibp.MS_F2_rd &&  ibp.R1_sr*3 > ibp.MS_R1_rd ) srscore=4;
  }
  
  // read depth are always available
  srscore=0;
  if ( ibp.F2_rd>0 && ibp.R1_rd>0 ) { 
    if ( ibp.F2_sr*8 > msc::rd[ibp.F2] || ibp.R1_sr*8 > msc::rd[ibp.R1] ) srscore=1;
    if ( (ibp.F2_sr*4 > msc::rd[ibp.F2] || ibp.R1_sr*4 > msc::rd[ibp.R1] ) &&
	 (ibp.F2_sr*8 > msc::rd[ibp.F2] && ibp.R1_sr*8 > msc::rd[ibp.R1] ) ) srscore=2;
    if ( (ibp.F2_sr*3 > msc::rd[ibp.F2] ||  ibp.R1_sr*3 > msc::rd[ibp.R1] ) &&
	 (ibp.F2_sr*4 > msc::rd[ibp.F2] && ibp.R1_sr*4 > msc::rd[ibp.R1] ) ) srscore=3;
    if ( ibp.F2_sr*3 > msc::rd[ibp.F2] &&  ibp.R1_sr*3 > msc::rd[ibp.R1] ) srscore=4;
  }
  
  
  // low coverage ignored
  if ( ibp.F2_rd<6 && ibp.R1_rd<6 && ibp.rd<6 ) rdscore=0;
  if ( ibp.F2_rp<6 && ibp.R1_rp<6 && ibp.FRrp<6 ) rpscore=0;
  if ( ibp.MS_ED>msc::bam_l_qseq/2 ) srscore=0;
  if ( ibp.F2_sr<=2 && ibp.R1_sr<=2 ) if ( srscore>0 ) srscore=0;
  if ( ibp.F2_sr<=2 || ibp.R1_sr<=2 ) if ( srscore>1 ) srscore=1;
  
  if ( ddscore==0 ) rdscore=0;
  
  // only update score for calculated values
  if ( ibp.F2_rp>=0 ) ibp.rpscore=rpscore;
  if ( ibp.F2_rd>=0 ) ibp.rdscore=rdscore;
  if ( ibp.F2_rd_100>=0 ) ibp.ddscore=ddscore;
  if ( ibp.F2_sr>=0 ) ibp.srscore=srscore;
  
  return;
}
void assess_rd_rp_sr_infomation(pairinfo_st& ibp)
{
  assess_rd_rp_sr_infomation(ibp, 0, 0) ;
  return;
} 

// make some dudgement based on collected information
void assess_rd_rp_sr_infomation(vector<pairinfo_st>& bp) 
{
  if ( bp.size()<1 ) return;
  
  // roughly approximate read pair and read depth
  vector<int> t1(0);
  for(size_t i=0; i<bp.size(); ++i) {
    if ( bp[i].F2_rd>9 ) t1.push_back(bp[i].F2_rd);
    if ( bp[i].R1_rd>9 ) t1.push_back(bp[i].R1_rd);
  }
  std::nth_element(t1.begin(), t1.begin() + t1.size()/2, t1.end());  
  int medRD= t1.size()>0 ? t1[t1.size()/2] : 0 ;
  
  t1.clear();
  for(size_t i=0; i<bp.size(); ++i) {
    if ( bp[i].F2_rp>9 ) t1.push_back(bp[i].F2_rp);
    if ( bp[i].R1_rp>9 ) t1.push_back(bp[i].R1_rp);
  }
  std::nth_element(t1.begin(), t1.begin() + t1.size()/2, t1.end());  
  int medRP= t1.size()>0 ? t1[t1.size()/2] : 0 ;
  
  for (size_t i=0; i<bp.size(); ++i) 
    assess_rd_rp_sr_infomation(bp[i], medRD, medRP);
  
  return;
  
}

