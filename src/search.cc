/*
  Copyright (C) 2014 Torbjorn Rognes

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  Contact: Torbjorn Rognes <torognes@ifi.uio.no>,
  Department of Informatics, University of Oslo,
  PO Box 1080 Blindern, NO-0316 Oslo, Norway
*/

#include "vsearch.h"

#define MINMATCHSAMPLECOUNT 6
#define MINMATCHSAMPLEFREQ (1/16)

//#define COMPARENONVECTORIZED

/* per thread data */
static struct searchinfo_s
{
  pthread_t pthread;            /* pthread info */
  long query_no;                /* query number, zero-based */
  long qsize;                   /* query abundance */
  long query_head_len;          /* query header length */
  long query_head_alloc;        /* bytes allocated for the header */
  char * query_head;            /* query header */
  long qseqlen;                 /* query length */
  long seq_alloc;               /* bytes allocated for the query sequence */
  long rc_seq_alloc;            /* bytes allocated for reverse complement q */
  char * qsequence;             /* query sequence */
  char * rc;                    /* query sequence, reverse complement */
  struct uhandle_s * uh;        /* unique kmer finder instance */
  unsigned int kmersamplecount; /* number of kmer samples from query */
  unsigned int * kmersample;    /* list of kmers sampled from query */
  unsigned int * targetlist;    /* list of db seqs with >0 kmer match */
  unsigned int * hitcount;      /* list of kmer counts for each db seq */
  int topcount;                 /* number of db seqs with kmer matches kept */
  struct nwinfo_s * nw;         /* NW aligner instance */
  struct hit * hits;            /* list of hits */
  int hit_count;                /* number of hits in the above list */
  struct s16info_s * s;         /* SIMD aligner instance */
  int strand;
  int accepts;
  int rejects;
  int candidatecount;
  int candidate_target[8];
  int candidate_kmercount[8];
  minheap_t * m;
} * sia;

/* global constants/data, no need for synchronization */
static long scorematrix[16][16];
static int tophits; /* the maximum number of hits to keep */
static int seqcount; /* number of database sequences */
static pthread_attr_t attr;

/* global data protected by mutex */
static pthread_mutex_t mutex_input;
static pthread_mutex_t mutex_output;
static long qmatches;
static long queries;
static int * dbmatched;
static FILE * fp_alnout = 0;
static FILE * fp_userout = 0;
static FILE * fp_blast6out = 0;
static FILE * fp_uc = 0;
static FILE * fp_fastapairs = 0;
static FILE * fp_matched = 0;
static FILE * fp_notmatched = 0;
static FILE * fp_dbmatched = 0;
static FILE * fp_dbnotmatched = 0;

int hit_compare(const void * a, const void * b)
{
  struct hit * x = (struct hit *) a;
  struct hit * y = (struct hit *) b;

  // high id, then low id
  // early target, then late target

  if (x->internal_id > y->internal_id)
    return -1;
  else if (x->internal_id < y->internal_id)
    return +1;
  else
    if (x->target < y->target)
      return -1;
    else if (x->target > y->target)
      return +1;
    else
      return 0;
}

void topscore_insert(int i, struct searchinfo_s * si)
{
  /*
    The current implementation uses a linear search and can be really
    slow in some cases.
    The list of top scores should probably be converted into
    a min heap (priority queue) for improved performance, see:
    http://blog.mischel.com/2011/10/25/when-theory-meets-practice/
  */
  
  unsigned int count = si->hitcount[i];
  
  /* ignore sequences with very few kmer matches */
  if (count < MINMATCHSAMPLECOUNT)
    return;
  if (count < MINMATCHSAMPLEFREQ * si->kmersamplecount)
    return;

  elem_t novel;
  novel.seqno = i;
  novel.count = count;
  novel.length = db_getsequencelen(i);

  minheap_add(si->m, & novel);
}


void search_topscores(struct searchinfo_s * si)
{
  /*
    Count the kmer hits in each database sequence and
    make a sorted list of a given number (th)
    of the database sequences with the highest number of matching kmers.
    These are stored in the si->topscores array.
  */

  /* count kmer hits in the database sequences */
  
  /* zero counts */
  memset(si->hitcount, 0, seqcount * sizeof(unsigned int));
  
  /* count total humber of hits and compute hit density */
  unsigned int totalhits = 0;
  for(unsigned int i=0; i<si->kmersamplecount; i++)
    {
      unsigned int kmer = si->kmersample[i];
      totalhits += kmerhash[kmer+1] - kmerhash[kmer];
    }
  double hitspertarget = totalhits * 1.0 / seqcount;

#if 0
  printf("Total hits: %u SeqCount: %u Hits/Target: %.2f\n", 
         totalhits, seqcount, hitspertarget);
#endif

  minheap_empty(si->m);

  si->topcount = 0;
  if (hitspertarget > 0.3)
    {
      /* dense hit distribution - check all targets - no need for a list */

      for(unsigned int i=0; i<si->kmersamplecount; i++)
        {
          unsigned int kmer = si->kmersample[i];
          unsigned int * a = kmerindex + kmerhash[kmer];
          unsigned int * b = kmerindex + kmerhash[kmer+1];
          for(unsigned int * j=a; j<b; j++)
            si->hitcount[*j]++;
        }

      for(int i=0; i < seqcount; i++)
        topscore_insert(i, si);
    }
  else
    {
      /* sparse hits - check only a list of targets */
      /* create list with targets (no duplications) */

      unsigned int targetcount = 0;
      for(unsigned int i=0; i<si->kmersamplecount; i++)
        {
          unsigned int kmer = si->kmersample[i];
          unsigned int * a = kmerindex + kmerhash[kmer];
          unsigned int * b = kmerindex + kmerhash[kmer+1];
          
          for(unsigned int * j=a; j<b; j++)
            {
              /* append to target list */
              if (si->hitcount[*j] == 0)
                si->targetlist[targetcount++] = *j;

              si->hitcount[*j]++;
            }
        }

      for(unsigned int z=0; z < targetcount; z++)
        topscore_insert(si->targetlist[z], si);
    }
}

int seqncmp(char * a, char * b, unsigned long n)
{
  for(unsigned int i = 0; i<n; i++)
    if (chrmap_4bit[(int)(a[i])] != chrmap_4bit[(int)(b[i])])
      return 1;
  return 0;
}


void align_candidates(struct searchinfo_s * si)
{
  /* compute global alignment */
  
  unsigned int target_list[8];
  CELL  nwscore_list[8];
  unsigned short nwalignmentlength_list[8];
  unsigned short nwmatches_list[8];
  unsigned short nwmismatches_list[8];
  unsigned short nwgaps_list[8];
  char * nwcigar_list[8];
  
  for(int i=0; i < si->candidatecount; i++)
    target_list[i] = si->candidate_target[i];
  
  search16(si->s,
           si->candidatecount,
           target_list,
           nwscore_list,
           nwalignmentlength_list,
           nwmatches_list,
           nwmismatches_list,
           nwgaps_list,
           nwcigar_list);
  
  for(int i = 0; i < si->candidatecount; i++)
    {
      unsigned int target = target_list[i];
      CELL  nwscore = nwscore_list[i];
      unsigned short nwalignmentlength = nwalignmentlength_list[i];
      unsigned short nwmatches = nwmatches_list[i];
      unsigned short nwmismatches = nwmismatches_list[i];
      unsigned short nwgaps = nwgaps_list[i];
      char * nwcigar = nwcigar_list[i];

#ifdef COMPARENONVECTORIZED
      /* compare results with non-vectorized nw */

      char * qseq;
      if (si->strand)
        qseq = si->rc;
      else
        qseq = si->qsequence;

      char * dseq = db_getsequence(target);
      long dseqlen = db_getsequencelen(target);

      long xnwscore;
      long xnwdiff;
      long xnwindels;
      long xnwgaps;
      long xnwalignmentlength;
      char * xnwcigar = 0;

      nw_align(dseq,
               dseq + dseqlen,
               qseq,
               qseq + si->qseqlen,
               (long*) scorematrix,
               opt_gap_open_query_left,
               opt_gap_open_query_interior,
               opt_gap_open_query_right,
               opt_gap_open_target_left,
               opt_gap_open_target_interior,
               opt_gap_open_target_right,
               opt_gap_extension_query_left,
               opt_gap_extension_query_interior,
               opt_gap_extension_query_right,
               opt_gap_extension_target_left,
               opt_gap_extension_target_interior,
               opt_gap_extension_target_right,
               & xnwscore,
               & xnwdiff,
               & xnwgaps,
               & xnwindels,
               & xnwalignmentlength,
               & xnwcigar,
               si->query_no,
               target,
               si->nw);

      if ((xnwscore != nwscore) || (strcmp(xnwcigar, nwcigar) != 0))
        {
          printf("Alignment error in channel %d:\n", i);
          printf("qlen, dlen: %ld, %ld\n",si->qseqlen, dseqlen);
          printf("qseq: [%s]\n", qseq);
          printf("dseq: [%s]\n", dseq);
          printf("Non-vectorized: %ld %s\n", xnwscore, xnwcigar);
          printf("Vectorized:     %d %s\n",   nwscore,  nwcigar);
        }
      
      free(xnwcigar);

#endif

      int kmercount = si->candidate_kmercount[i];

      int nwdiff = nwalignmentlength - nwmatches;
      int nwindels = nwalignmentlength - nwmatches - nwmismatches;
      char * nwalignment = nwcigar;
      
      double nwid = (nwalignmentlength - nwdiff) * 100.0 / nwalignmentlength;
      
      /* info for semi-global alignment (without gaps at ends) */
      
      long trim_aln_left = 0;
      long trim_q_left = 0;
      long trim_t_left = 0;
      long trim_aln_right = 0;
      long trim_q_right = 0;
      long trim_t_right = 0;
      
      /* left trim alignment */
      
      char * p = nwalignment;
      long run = 1;
      int scanlength = 0;
      sscanf(p, "%ld%n", &run, &scanlength);
      char op = *(p+scanlength);
      if (op != 'M')
        {
          trim_aln_left = 1 + scanlength;
          if (op == 'D')
            trim_q_left = run;
          else
            trim_t_left = run;
        }
      
      /* right trim alignment */
      
      char * e = nwalignment + strlen(nwalignment);
      p = e - 1;
      op = *p;
      if (op != 'M')
        {
          while (*(p-1) <= '9')
            p--;
          run = 1;
          sscanf(p, "%ld", &run);
          trim_aln_right = e - p;
          if (op == 'D')
            trim_q_right = run;
          else
            trim_t_right = run;
        }
      
      long mismatches = nwdiff - nwindels;
      long matches = nwalignmentlength - nwdiff;
      long internal_alignmentlength = nwalignmentlength
        - trim_q_left - trim_t_left - trim_q_right - trim_t_right;
      long internal_gaps = nwgaps
        - (trim_q_left  + trim_t_left  > 0 ? 1 : 0)
        - (trim_q_right + trim_t_right > 0 ? 1 : 0);
      long internal_indels = nwindels
        - trim_q_left - trim_t_left - trim_q_right - trim_t_right;
      double internal_id = 100.0 * matches / internal_alignmentlength;
      
      /* test accept/reject criteria after alignment */

      if (/* weak_id */
          (internal_id >= 100.0 * opt_weak_id) &&
          /* maxsubs */
          (mismatches <= opt_maxsubs) &&
          /* maxgaps */
          (internal_gaps <= opt_maxgaps) &&
          /* mincols */
          (internal_alignmentlength >= opt_mincols) &&
          /* leftjust */
          ((!opt_leftjust) || (trim_q_left+trim_t_left == 0)) &&
          /* rightjust */
          ((!opt_rightjust) || (trim_q_right+trim_t_right == 0)) &&
          /* query_cov */
          (internal_alignmentlength >= opt_query_cov * si->qseqlen) &&
          /* target_cov */
          (internal_alignmentlength >= opt_target_cov *  
           db_getsequencelen(target)) &&
          /* maxid */
          (internal_id <= 100.0 * opt_maxid) &&
          /* mid */
          (100.0 * matches / (matches + mismatches) >= opt_mid) &&
          /* maxdiffs */
          (mismatches + internal_indels <= opt_maxdiffs))
        {
          si->hits[si->hit_count].target = target;
          si->hits[si->hit_count].count = kmercount;
          si->hits[si->hit_count].nwscore = nwscore;
          si->hits[si->hit_count].nwdiff = nwdiff;
          si->hits[si->hit_count].nwgaps = nwgaps;
          si->hits[si->hit_count].nwindels = nwindels;
          si->hits[si->hit_count].nwalignmentlength = nwalignmentlength;
          si->hits[si->hit_count].nwalignment = nwalignment;
          si->hits[si->hit_count].nwid = nwid;
          si->hits[si->hit_count].strand = si->strand;
          si->hits[si->hit_count].matches = matches;
          si->hits[si->hit_count].mismatches = mismatches;
          si->hits[si->hit_count].trim_q_left = trim_q_left;
          si->hits[si->hit_count].trim_q_right = trim_q_right;
          si->hits[si->hit_count].trim_t_left = trim_t_left;
          si->hits[si->hit_count].trim_t_right = trim_t_right;
          si->hits[si->hit_count].trim_aln_left = trim_aln_left;
          si->hits[si->hit_count].trim_aln_right = trim_aln_right;
          si->hits[si->hit_count].internal_alignmentlength =
            internal_alignmentlength;
          si->hits[si->hit_count].internal_gaps = internal_gaps;
          si->hits[si->hit_count].internal_indels = internal_indels;
          si->hits[si->hit_count].internal_id = internal_id;

          si->hit_count++;
          
          if (internal_id >= 100.0 * opt_id)
            si->accepts++;
        }
      else
        {
          free(nwalignment);
          si->rejects++;
        }
      
#if 0
      if ((si->accepts >= maxaccepts) || (si->rejects >= maxrejects))
        return;
#endif
    }
}

int search_onequery(struct searchinfo_s * si)
{
  si->hit_count = 0;

  for(int s = 0; s < opt_strand; s++)
    {
      si->strand = s;

      /* check plus or both strands*/
      /* s=0: plus; s=1: minus */
      char * qseq;
      if (s)
        qseq = si->rc;
      else
        qseq = si->qsequence;

      search16_qprep(si->s, qseq, si->qseqlen);

      /* extract unique kmer samples from query*/
      unique_count(si->uh, wordlength, 
                   si->qseqlen, qseq,
                   & si->kmersamplecount, & si->kmersample);
      
      /* find database sequences with the most kmer hits */
      search_topscores(si);
      
      /* analyse targets with the highest number of kmer hits */
      si->accepts = 0;
      si->rejects = 0;

      si->candidatecount = 0;

      minheap_sort(si->m);

      for(int t = 0;
          (si->accepts < maxaccepts) && 
            (si->rejects <= maxrejects) &&
            (!minheap_isempty(si->m));
          t++)
        {
          elem_t e = minheap_poplast(si->m);
          unsigned int target = e.seqno;
          unsigned int count = e.count;
          char * dlabel = db_getheader(target);
          char * dseq = db_getsequence(target);
          long dseqlen = db_getsequencelen(target);
          long tsize = db_getabundance(target);
          
          /* Test these accept/reject criteria before alignment */
          
          if (/* maxqsize */
              (si->qsize > opt_maxqsize) ||
              /* mintsize */
              (tsize < opt_mintsize) ||
              /* minsizeratio */
              (si->qsize < opt_minsizeratio * tsize) ||
              /* maxsizeratio */
              (si->qsize > opt_maxsizeratio * tsize) ||
              /* minqt */
              (si->qseqlen < opt_minqt * dseqlen) ||
              /* maxqt */
              (si->qseqlen > opt_maxqt * dseqlen) ||
              /* minsl */
              (si->qseqlen < dseqlen ? 
               si->qseqlen < opt_minsl * dseqlen : 
               dseqlen < opt_minsl * si->qseqlen) ||
              /* maxsl */
              (si->qseqlen < dseqlen ? 
               si->qseqlen > opt_maxsl * dseqlen : 
               dseqlen > opt_maxsl * si->qseqlen) ||
              /* idprefix */
              ((si->qseqlen < opt_idprefix) || 
               (dseqlen < opt_idprefix) ||
               (seqncmp(qseq,
                        dseq,
                        opt_idprefix))) ||
              /* idsuffix */
              ((si->qseqlen < opt_idsuffix) ||
               (dseqlen < opt_idsuffix) ||
               (seqncmp(qseq+si->qseqlen-opt_idsuffix,
                        dseq+dseqlen-opt_idsuffix,
                        opt_idsuffix))) ||
              /* self */
              (opt_self && (strcmp(si->query_head, dlabel) == 0)) ||
              /* selfid */
              (opt_selfid && (si->qseqlen == dseqlen) && 
               (seqncmp(qseq, dseq, si->qseqlen) == 0)))
            {
              si->rejects++;
            }
          else
            {
              si->candidate_target[si->candidatecount] = target;
              si->candidate_kmercount[si->candidatecount] = count;
              si->candidatecount++;
              if (si->candidatecount == 8)
                {
                  align_candidates(si);
                  si->candidatecount = 0;
                }
            }
        }  

      if (si->candidatecount > 0)
        {
          align_candidates(si);
          si->candidatecount = 0;
        }
    }
    
  /* sort and return hits */
  
  qsort(si->hits, si->hit_count, sizeof(struct hit), hit_compare);

  return si->hit_count;
}


void search_thread_run(struct searchinfo_s * si)
{
  while (1)
    {
      pthread_mutex_lock(&mutex_input);

      char * qhead;
      char * qseq;

      int r = query_getnext(& qhead, & si->query_head_len,
                            & qseq, & si->qseqlen,
                            & si->query_no, & si->qsize,
                            opt_qmask != MASK_SOFT);
      
      if (!r)
        {
          pthread_mutex_unlock(&mutex_input);
          break;
        }

      /* allocate more memory for header and sequence, if necessary */
      
      if (si->query_head_len + 1 > si->query_head_alloc)
        {
          si->query_head_alloc = si->query_head_len + 2001;
          si->query_head = (char*) xrealloc(si->query_head,
                                            (size_t)(si->query_head_alloc));
        }
      
      if (si->qseqlen + 1 > si->seq_alloc)
        {
          si->seq_alloc = si->qseqlen + 2001;
          si->qsequence = (char*) xrealloc(si->qsequence,
                                           (size_t)(si->seq_alloc));
        }
      
      /* copy header and sequence */
      strcpy(si->query_head, qhead);
      strcpy(si->qsequence, qseq);
    
      /* let other threads read input */
      pthread_mutex_unlock(&mutex_input);

      /* mask query */
      if (opt_qmask == MASK_DUST)
        {
          dust(si->qsequence, si->qseqlen);
        }
      else if ((opt_qmask == MASK_SOFT) && (opt_hardmask))
        {
          hardmask(si->qsequence, si->qseqlen);
        }

      /* compute reverse complement query sequence */
      if (opt_strand > 1)
        {
          if (si->qseqlen + 1 > si->rc_seq_alloc)
            {
              si->rc_seq_alloc = si->qseqlen + 2001;
              si->rc = (char *) xrealloc(si->rc, (size_t)(si->rc_seq_alloc));
            }
          reverse_complement(si->rc, si->qsequence, si->qseqlen);
        }
      
      /* perform search */
      int hit_count = search_onequery(si);
      
      /* show results */
      long toreport = MIN(opt_maxhits, hit_count);
      if (opt_weak_id == opt_id)
        toreport = MIN(toreport, maxaccepts);

      pthread_mutex_lock(&mutex_output);
      queries++;
      if (hit_count > 0)
        qmatches++;
      if (toreport)
        {
          if (fp_alnout)
            results_show_alnout(fp_alnout,
                                si->hits, toreport, si->query_head,
                                si->qsequence, si->qseqlen, si->rc);
      
          double top_hit_id = si->hits[0].internal_id;
          
          for(int t = 0; t < toreport; t++)
            {
              struct hit * hp = si->hits + t;
              
              if (opt_top_hits_only && (hp->internal_id < top_hit_id))
                break;
              
              if (fp_fastapairs)
                results_show_fastapairs_one(fp_fastapairs, hp, 
                                            si->query_head,
                                            si->qsequence,
                                            si->qseqlen,
                                            si->rc);

              if (fp_uc)
                if (opt_uc_allhits || (t==0))
                  results_show_uc_one(fp_uc, hp, si->query_head,
                                      si->qsequence, si->qseqlen, si->rc);
              
              if (fp_userout)
                results_show_userout_one(fp_userout, hp, si->query_head, 
                                         si->qsequence, si->qseqlen, si->rc);
              
              if (fp_blast6out)
                results_show_blast6out_one(fp_blast6out, hp, si->query_head,
                                           si->qsequence, si->qseqlen, si->rc);
            }
        }
      else
        {
          if (fp_uc)
            results_show_uc_one(fp_uc, 0, si->query_head,
                                si->qsequence, si->qseqlen, si->rc);
          
          if (opt_output_no_hits)
            {
              if (fp_userout)
                results_show_userout_one(fp_userout, 0, si->query_head, 
                                         si->qsequence, si->qseqlen, si->rc);
              
              if (fp_blast6out)
                results_show_blast6out_one(fp_blast6out, 0, si->query_head,
                                           si->qsequence, si->qseqlen, si->rc);
            }
        }

      if (hit_count)
        {
          if (opt_matched)
            {
              fprintf(fp_matched, ">%s\n", si->query_head);
              fprint_fasta_seq_only(fp_matched, si->qsequence, si->qseqlen,
                                    opt_fasta_width);
            }
        }
      else
        {
          if (opt_notmatched)
            {
              fprintf(fp_notmatched, ">%s\n", si->query_head);
              fprint_fasta_seq_only(fp_notmatched, si->qsequence, si->qseqlen,
                                    opt_fasta_width);
            }
        }

      /* update matching db sequences */
      for (long i=0; i<hit_count; i++)
        if (si->hits[i].internal_id >= 100.0 * opt_id)
          dbmatched[si->hits[i].target]++;
      
      progress_update(query_getfilepos());

      pthread_mutex_unlock(&mutex_output);

      /* free memory for alignment strings */
      for(int i=0; i<hit_count; i++)
        free(si->hits[i].nwalignment);
    }
}

void search_thread_init(struct searchinfo_s * si)
{
  /* thread specific initialiation */
  si->uh = unique_init();
  si->hitcount = (unsigned int *) xmalloc
    (seqcount * sizeof(unsigned int));
  si->m = minheap_init(tophits);
  si->targetlist = (unsigned int*) xmalloc
    (sizeof(unsigned int)*seqcount);
  si->hits = (struct hit *) xmalloc
    (sizeof(struct hit) * (tophits) * opt_strand);
  si->qsize = 1;
  si->query_head_alloc = 0;
  si->query_head = 0;
  si->seq_alloc = 0;
  si->qsequence = 0;
  si->rc_seq_alloc = 0;
  si->rc = 0;
#ifdef COMPARENONVECTORIZED
  si->nw = nw_init();
#else
  si->nw = 0;
#endif
  si->s = search16_init(match_score,
                        mismatch_score,
                        opt_gap_open_query_left,
                        opt_gap_open_target_left,
                        opt_gap_open_query_interior,
                        opt_gap_open_target_interior,
                        opt_gap_open_query_right,
                        opt_gap_open_target_right,
                        opt_gap_extension_query_left,
                        opt_gap_extension_target_left,
                        opt_gap_extension_query_interior,
                        opt_gap_extension_target_interior,
                        opt_gap_extension_query_right,
                        opt_gap_extension_target_right);
}

void search_thread_exit(struct searchinfo_s * si)
{
  /* thread specific clean up */
  search16_exit(si->s);
#ifdef COMPARENONVECTORIZED
  nw_exit(si->nw);
#endif
  unique_exit(si->uh);
  free(si->targetlist);
  free(si->hits);
  minheap_exit(si->m);
  free(si->hitcount);
  if (si->query_head)
    free(si->query_head);
  if (si->qsequence)
    free(si->qsequence);
  if (si->rc)
    free(si->rc);
}

void * search_thread_worker(void * vp)
{
  long t = (long) vp;
  struct searchinfo_s * si = sia + t;
  search_thread_run(si);
  return 0;
}

void search_thread_worker_init()
{
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  
  /* allocate memory for thread info */
  sia = (struct searchinfo_s *) xmalloc(opt_threads * 
                                        sizeof(struct searchinfo_s));
  
  /* init and create worker threads, put them into stand-by mode */
  for(int t=0; t<opt_threads; t++)
    {
      struct searchinfo_s * si = sia + t;
      search_thread_init(si);
      if (pthread_create(&si->pthread, &attr, search_thread_worker,
                         (void*)(long)t))
        fatal("Cannot create thread");
    }
}

void search_thread_worker_exit()
{
  /* finish and clean up worker threads */
  for(int t=0; t<opt_threads; t++)
    {
      struct searchinfo_s * si = sia + t;
      
      /* wait for worker to quit */
      if (pthread_join(si->pthread, NULL))
        fatal("Cannot join thread");

      search_thread_exit(si);
    }

  free(sia);

  pthread_attr_destroy(&attr);
}

void search(char * cmdline, char * progheader)
{
  /* open output files */

  if (alnoutfilename)
    {
      fp_alnout = fopen(alnoutfilename, "w");
      if (! fp_alnout)
        fatal("Unable to open alignment output file for writing");
    }

  if (useroutfilename)
    {
      fp_userout = fopen(useroutfilename, "w");
      if (! fp_userout)
        fatal("Unable to open user-defined output file for writing");
    }

  if (blast6outfilename)
    {
      fp_blast6out = fopen(blast6outfilename, "w");
      if (! fp_blast6out)
        fatal("Unable to open blast6-like output file for writing");
    }

  if (ucfilename)
    {
      fp_uc = fopen(ucfilename, "w");
      if (! fp_uc)
        fatal("Unable to open uclust-like output file for writing");
    }

  if (opt_fastapairs)
    {
      fp_fastapairs = fopen(opt_fastapairs, "w");
      if (! fp_fastapairs)
        fatal("Unable to open fastapairs output file for writing");
    }

  if (opt_matched)
    {
      fp_matched = fopen(opt_matched, "w");
      if (! fp_matched)
        fatal("Unable to open matched output file for writing");
    }

  if (opt_notmatched)
    {
      fp_notmatched = fopen(opt_notmatched, "w");
      if (! fp_notmatched)
        fatal("Unable to open notmatched output file for writing");
    }

  if (opt_dbmatched)
    {
      fp_dbmatched = fopen(opt_dbmatched, "w");
      if (! fp_dbmatched)
        fatal("Unable to open dbmatched output file for writing");
    }

  if (opt_dbnotmatched)
    {
      fp_dbnotmatched = fopen(opt_dbnotmatched, "w");
      if (! fp_dbnotmatched)
        fatal("Unable to open dbnotmatched output file for writing");
    }

  if (alnoutfilename)
    {
      fprintf(fp_alnout, "%s\n", cmdline);
      fprintf(fp_alnout, "%s\n", progheader);
    }

  db_read(databasefilename, opt_dbmask != MASK_SOFT);

  if (opt_dbmask == MASK_DUST)
    dust_all();
  else if ((opt_dbmask == MASK_SOFT) && (opt_hardmask))
    hardmask_all();

  show_rusage();

  dbindex_build();

  seqcount = db_getsequencecount();

  if ((maxrejects == 0) || (maxrejects > seqcount))
    maxrejects = seqcount;

  if (maxaccepts > seqcount)
    maxaccepts = seqcount;

  tophits = maxrejects + maxaccepts + 8;
  if (tophits > seqcount)
    tophits = seqcount;
  
  qmatches = 0;
  queries = 0;

  for(int i=0; i<16; i++)
    for(int j=0; j<16; j++)
      if (i==j)
        scorematrix[i][j] = match_score;
      else if ((i>3) || (j>3))
        scorematrix[i][j] = 0;
      else
        scorematrix[i][j] = mismatch_score;
  
  query_open(opt_usearch_global);

  dbmatched = (int*) xmalloc(seqcount * sizeof(int*));
  memset(dbmatched, 0, seqcount * sizeof(int*));

  pthread_mutex_init(&mutex_input, NULL);
  pthread_mutex_init(&mutex_output, NULL);

  progress_init("Searching", query_getfilesize());
  search_thread_worker_init();
  search_thread_worker_exit();
  progress_done();

  fprintf(stderr, "Matching query sequences: %ld of %ld (%.2f%%)\n", 
          qmatches, queries, 100.0 * qmatches / queries);
  if (opt_dbmatched || opt_dbnotmatched)
    {
      for(long i=0; i<seqcount; i++)
        if (dbmatched[i])
          {
            if (opt_dbmatched)
              db_fprint_fasta_with_size(fp_dbmatched, i, dbmatched[i]);
          }
        else
          {
            if (opt_dbnotmatched)
              db_fprint_fasta(fp_dbnotmatched, i);
          }
    }
  
  /* clean up, global */
  free(dbmatched);
  query_close();
  dbindex_free();
  db_free();
  pthread_mutex_destroy(&mutex_output);
  pthread_mutex_destroy(&mutex_input);
  if (opt_matched)
    fclose(fp_matched);
  if (opt_notmatched)
    fclose(fp_notmatched);
  if (opt_dbmatched)
    fclose(fp_dbmatched);
  if (opt_dbnotmatched)
    fclose(fp_dbnotmatched);
  if (opt_fastapairs)
    fclose(fp_fastapairs);
  if (fp_uc)
    fclose(fp_uc);
  if (fp_blast6out)
    fclose(fp_blast6out);
  if (fp_userout)
    fclose(fp_userout);
  if (fp_alnout)
    fclose(fp_alnout);
  show_rusage();
}
