/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2008 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced 
 * Research Projects Agency and the National Science Foundation of the 
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/**
 * @file ps_lattice.c Word graph search.
 */

/* System headers. */
#include <assert.h>
#include <string.h>

/* SphinxBase headers. */
#include <ckd_alloc.h>
#include <listelem_alloc.h>

/* Local headers. */
#include "pocketsphinx.h" /* To make sure ps_lattice_write() gets exported */
#include "ps_lattice.h"
#include "ngram_search.h"

/*
 * Create a directed link between "from" and "to" nodes, but if a link already exists,
 * choose one with the best ascr.
 */
void
ps_lattice_link(ps_lattice_t *dag, latnode_t *from, latnode_t *to, int32 score, int32 ef)
{
    latlink_list_t *fwdlink;

    /* Look for an existing link between "from" and "to" nodes */
    for (fwdlink = from->exits; fwdlink; fwdlink = fwdlink->next)
        if (fwdlink->link->to == to)
            break;

    if (fwdlink == NULL) {
        latlink_list_t *revlink;
        latlink_t *link;

        /* No link between the two nodes; create a new one */
        link = listelem_malloc(dag->latlink_alloc);
        fwdlink = listelem_malloc(dag->latlink_list_alloc);
        revlink = listelem_malloc(dag->latlink_list_alloc);

        link->from = from;
        link->to = to;
        link->ascr = score;
        link->ef = ef;
        link->best_prev = NULL;

        fwdlink->link = revlink->link = link;
        fwdlink->next = from->exits;
        from->exits = fwdlink;
        revlink->next = to->entries;
        to->entries = revlink;
    }
    else {
        /* Link already exists; just retain the best ascr */
        if (fwdlink->link->ascr < score) {
            fwdlink->link->ascr = score;
            fwdlink->link->ef = ef;
        }
    }
}

void
ps_lattice_bypass_fillers(ps_lattice_t *dag, int32 silpen, int32 fillpen)
{
    latnode_t *node;
    int32 score;

    /* Bypass filler nodes */
    for (node = dag->nodes; node; node = node->next) {
        latlink_list_t *revlink;
        if (node == dag->end || !ISA_FILLER_WORD(dag->search, node->basewid))
            continue;

        /* Replace each link entering filler node with links to all its successors */
        for (revlink = node->entries; revlink; revlink = revlink->next) {
            latlink_list_t *forlink;
            latlink_t *rlink = revlink->link;

            score = (node->basewid == ps_search_silence_wid(dag->search)) ? silpen : fillpen;
            score += rlink->ascr;
            /*
             * Make links from predecessor of filler (from) to successors of filler.
             * But if successor is a filler, it has already been eliminated since it
             * appears earlier in latnode_list (see build...).  So it can be skipped.
             */
            for (forlink = node->exits; forlink; forlink = forlink->next) {
                latlink_t *flink = forlink->link;
                if (!ISA_FILLER_WORD(dag->search, flink->to->basewid)) {
                    ps_lattice_link(dag, rlink->from, flink->to,
                                    score + flink->ascr, flink->ef);
                }
            }
        }
    }
}

static void
delete_node(ps_lattice_t *dag, latnode_t *node)
{
    latlink_list_t *x, *next_x;

    for (x = node->exits; x; x = next_x) {
        next_x = x->next;
        x->link->from = NULL;
        listelem_free(dag->latlink_list_alloc, x);
    }
    for (x = node->entries; x; x = next_x) {
        next_x = x->next;
        x->link->to = NULL;
        listelem_free(dag->latlink_list_alloc, x);
    }
    listelem_free(dag->latnode_alloc, node);
}

static void
remove_dangling_links(ps_lattice_t *dag, latnode_t *node)
{
    latlink_list_t *x, *prev_x, *next_x;

    prev_x = NULL;
    for (x = node->exits; x; x = next_x) {
        next_x = x->next;
        if (x->link->to == NULL) {
            if (prev_x)
                prev_x->next = next_x;
            else
                node->exits = next_x;
            listelem_free(dag->latlink_alloc, x->link);
            listelem_free(dag->latlink_list_alloc, x);
        }
        else
            prev_x = x;
    }
    prev_x = NULL;
    for (x = node->entries; x; x = next_x) {
        next_x = x->next;
        if (x->link->from == NULL) {
            if (prev_x)
                prev_x->next = next_x;
            else
                node->exits = next_x;
            listelem_free(dag->latlink_alloc, x->link);
            listelem_free(dag->latlink_list_alloc, x);
        }
        else
            prev_x = x;
    }
}

void
ps_lattice_delete_unreachable(ps_lattice_t *dag)
{
    latnode_t *node, *prev_node, *next_node;
    int i;

    /* Remove unreachable nodes from the list of nodes. */
    prev_node = NULL;
    for (node = dag->nodes; node; node = next_node) {
        next_node = node->next;
        if (!node->reachable) {
            if (prev_node)
                prev_node->next = next_node;
            else
                dag->nodes = next_node;
            /* Delete this node and NULLify links to it. */
            delete_node(dag, node);
        }
        else
            prev_node = node;
    }

    /* Remove all links to and from unreachable nodes. */
    i = 0;
    for (node = dag->nodes; node; node = node->next) {
        /* Assign sequence numbers. */
        node->id = i++;

        /* We should obviously not encounter unreachable nodes here! */
        assert(node->reachable);

        /* Remove all links that go nowhere. */
        remove_dangling_links(dag, node);
    }
}

int32
ps_lattice_write(ps_lattice_t *dag, char const *filename)
{
    FILE *fp;
    int32 i;
    latnode_t *d, *initial, *final;

    initial = dag->start;
    final = dag->end;

    E_INFO("Writing lattice file: %s\n", filename);
    if ((fp = fopen(filename, "w")) == NULL) {
        E_ERROR("fopen(%s,w) failed\n", filename);
        return -1;
    }

    fprintf(fp,
            "# Generated by PocketSphinx\n");
    fprintf(fp, "# -logbase %e\n",
            cmd_ln_float32_r(ps_search_config(dag->search), "-logbase"));
    fprintf(fp, "#\n");

    fprintf(fp, "Frames %d\n", dag->n_frames);
    fprintf(fp, "#\n");

    for (i = 0, d = dag->nodes; d; d = d->next, i++);
    fprintf(fp,
            "Nodes %d (NODEID WORD STARTFRAME FIRST-ENDFRAME LAST-ENDFRAME)\n",
            i);
    for (i = 0, d = dag->nodes; d; d = d->next, i++) {
        d->id = i;
        fprintf(fp, "%d %s %d %d %d\n",
                i, dict_word_str(ps_search_dict(dag->search), d->wid),
                d->sf, d->fef, d->lef);
    }
    fprintf(fp, "#\n");

    fprintf(fp, "Initial %d\nFinal %d\n", initial->id, final->id);
    fprintf(fp, "#\n");

    /* Don't bother with this, it's not used by anything. */
    fprintf(fp, "BestSegAscr %d (NODEID ENDFRAME ASCORE)\n",
            0 /* #BPTable entries */ );
    fprintf(fp, "#\n");

    fprintf(fp, "Edges (FROM-NODEID TO-NODEID ASCORE)\n");
    for (d = dag->nodes; d; d = d->next) {
        latlink_list_t *l;
        for (l = d->exits; l; l = l->next)
            fprintf(fp, "%d %d %d\n",
                    d->id, l->link->to->id, l->link->ascr);
    }
    fprintf(fp, "End\n");
    fclose(fp);

    return 0;
}

ps_lattice_t *
ps_lattice_init(ps_search_t *search, int n_frame)
{
    ps_lattice_t *dag;

    dag = ckd_calloc(1, sizeof(*dag));
    dag->search = search;
    dag->n_frames = n_frame;
    dag->latnode_alloc = listelem_alloc_init(sizeof(latnode_t));
    dag->latlink_alloc = listelem_alloc_init(sizeof(latlink_t));
    dag->latlink_list_alloc = listelem_alloc_init(sizeof(latlink_list_t));
    return dag;
}

void
ps_lattice_free(ps_lattice_t *dag)
{
    if (dag == NULL)
        return;
    listelem_alloc_free(dag->latnode_alloc);
    listelem_alloc_free(dag->latlink_alloc);
    listelem_alloc_free(dag->latlink_list_alloc);
    ckd_free(dag->hyp_str);
    ckd_free(dag);
}

char const *
ps_lattice_hyp(ps_lattice_t *dag, latlink_t *link)
{
    latlink_t *l;
    size_t len;
    char *c;

    /* Backtrace once to get hypothesis length. */
    len = 0;
    if (ISA_REAL_WORD(dag->search, link->to->basewid))
        len += strlen(dict_word_str(ps_search_dict(dag->search), link->to->basewid)) + 1;
    for (l = link; l; l = l->best_prev) {
        if (ISA_REAL_WORD(dag->search, l->from->basewid))
            len += strlen(dict_word_str(ps_search_dict(dag->search), l->from->basewid)) + 1;
    }

    /* Backtrace again to construct hypothesis string. */
    ckd_free(dag->hyp_str);
    dag->hyp_str = ckd_calloc(1, len);
    c = dag->hyp_str + len - 1;
    if (ISA_REAL_WORD(dag->search, link->to->basewid)) {
        len = strlen(dict_word_str(ps_search_dict(dag->search), link->to->basewid));
        c -= len;
        memcpy(c, dict_word_str(ps_search_dict(dag->search), link->to->basewid), len);
        if (c > dag->hyp_str) {
            --c;
            *c = ' ';
        }
    }
    for (l = link; l; l = l->best_prev) {
        if (ISA_REAL_WORD(dag->search, l->from->basewid)) {
            len = strlen(dict_word_str(ps_search_dict(dag->search), l->from->basewid));
            c -= len;
            memcpy(c, dict_word_str(ps_search_dict(dag->search), l->from->basewid), len);
            if (c > dag->hyp_str) {
                --c;
                *c = ' ';
            }
        }
    }

    return dag->hyp_str;
}

static void
ps_lattice_compute_lscr(ps_seg_t *seg, latlink_t *link, int to)
{
    ngram_model_t *lmset;

    /* Language model score is included in the link score for FSG
     * search.  FIXME: Of course, this is sort of a hack :( */
    if (0 != strcmp(ps_search_name(seg->search), "ngram")) {
        seg->lback = 1; /* Unigram... */
        seg->lscr = 0;
        return;
    }
        
    lmset = ((ngram_search_t *)seg->search)->lmset;

    if (link->best_prev == NULL) {
        if (to) /* Sentence has only two words. */
            seg->lscr = ngram_bg_score(lmset, link->to->basewid,
                                       link->from->basewid, &seg->lback);
        else {/* This is the start symbol, its lscr is always 0. */
            seg->lscr = 0;
            seg->lback = 1;
        }
    }
    else {
        /* Find the two predecessor words. */
        if (to) {
            seg->lscr = ngram_tg_score(lmset, link->to->basewid,
                                       link->from->basewid,
                                       link->best_prev->from->basewid,
                                       &seg->lback);
        }
        else {
            if (link->best_prev->best_prev)
                seg->lscr = ngram_tg_score(lmset, link->from->basewid,
                                           link->best_prev->from->basewid,
                                           link->best_prev->best_prev->from->basewid,
                                           &seg->lback);
            else
                seg->lscr = ngram_bg_score(lmset, link->from->basewid,
                                           link->best_prev->from->basewid,
                                           &seg->lback);
        }
    }
}

static void
ps_lattice_link2itor(ps_seg_t *seg, latlink_t *link, int to)
{
    dag_seg_t *itor = (dag_seg_t *)seg;
    latnode_t *node;

    if (to) {
        node = link->to;
        seg->ef = node->lef;
    }
    else {
        node = link->from;
        seg->ef = link->ef;
    }
    seg->word = dict_word_str(ps_search_dict(seg->search), node->wid);
    seg->sf = node->sf;
    seg->prob = link->alpha + link->beta - itor->norm;
    seg->ascr = link->ascr;
    /* Compute language model score from best predecessors. */
    ps_lattice_compute_lscr(seg, link, to);
}

static void
ps_lattice_seg_free(ps_seg_t *seg)
{
    dag_seg_t *itor = (dag_seg_t *)seg;
    
    ckd_free(itor->links);
    ckd_free(itor);
}

static ps_seg_t *
ps_lattice_seg_next(ps_seg_t *seg)
{
    dag_seg_t *itor = (dag_seg_t *)seg;

    ++itor->cur;
    if (itor->cur == itor->n_links + 1) {
        ps_lattice_seg_free(seg);
        return NULL;
    }
    else if (itor->cur == itor->n_links) {
        /* Re-use the last link but with the "to" node. */
        ps_lattice_link2itor(seg, itor->links[itor->cur - 1], TRUE);
    }
    else {
        ps_lattice_link2itor(seg, itor->links[itor->cur], FALSE);
    }

    return seg;
}

static ps_segfuncs_t ps_lattice_segfuncs = {
    /* seg_next */ ps_lattice_seg_next,
    /* seg_free */ ps_lattice_seg_free
};

ps_seg_t *
ps_lattice_seg_iter(ps_lattice_t *dag, latlink_t *link, float32 lwf)
{
    dag_seg_t *itor;
    latlink_t *l;
    int cur;

    /* Calling this an "iterator" is a bit of a misnomer since we have
     * to get the entire backtrace in order to produce it.
     */
    itor = ckd_calloc(1, sizeof(*itor));
    itor->base.vt = &ps_lattice_segfuncs;
    itor->base.search = dag->search;
    itor->base.lwf = lwf;
    itor->n_links = 0;
    itor->norm = dag->norm;

    for (l = link; l; l = l->best_prev) {
        ++itor->n_links;
    }
    if (itor->n_links == 0) {
        ckd_free(itor);
        return NULL;
    }

    itor->links = ckd_calloc(itor->n_links, sizeof(*itor->links));
    cur = itor->n_links - 1;
    for (l = link; l; l = l->best_prev) {
        itor->links[cur] = l;
        --cur;
    }

    ps_lattice_link2itor((ps_seg_t *)itor, itor->links[0], FALSE);
    return (ps_seg_t *)itor;
}

latlink_list_t *
latlink_list_new(ps_lattice_t *dag, latlink_t *link, latlink_list_t *next)
{
    latlink_list_t *ll;

    ll = listelem_malloc(dag->latlink_list_alloc);
    ll->link = link;
    ll->next = next;

    return ll;
}

void
ps_lattice_pushq(ps_lattice_t *dag, latlink_t *link)
{
    if (dag->q_head == NULL)
        dag->q_head = dag->q_tail = latlink_list_new(dag, link, NULL);
    else {
        dag->q_tail->next = latlink_list_new(dag, link, NULL);
        dag->q_tail = dag->q_tail->next;
    }

}

latlink_t *
ps_lattice_popq(ps_lattice_t *dag)
{
    latlink_list_t *x;
    latlink_t *link;

    if (dag->q_head == NULL)
        return NULL;
    link = dag->q_head->link;
    x = dag->q_head->next;
    listelem_free(dag->latlink_list_alloc, dag->q_head);
    dag->q_head = x;
    if (dag->q_head == NULL)
        dag->q_tail = NULL;
    return link;
}

void
ps_lattice_delq(ps_lattice_t *dag)
{
    while (ps_lattice_popq(dag)) {
        /* Do nothing. */
    }
}

latlink_t *
ps_lattice_traverse_edges(ps_lattice_t *dag, latnode_t *start, latnode_t *end)
{
    latnode_t *node;
    latlink_list_t *x;

    /* Cancel any unfinished traversal. */
    ps_lattice_delq(dag);

    /* Initialize node fanin counts and path scores. */
    for (node = dag->nodes; node; node = node->next)
        node->info.fanin = 0;
    for (node = dag->nodes; node; node = node->next) {
        for (x = node->exits; x; x = x->next)
            (x->link->to->info.fanin)++;
    }

    /* Initialize agenda with all exits from start. */
    if (start == NULL) start = dag->start;
    for (x = start->exits; x; x = x->next)
        ps_lattice_pushq(dag, x->link);

    /* Pull the first edge off the queue. */
    return ps_lattice_traverse_next(dag, end);
}

latlink_t *
ps_lattice_traverse_next(ps_lattice_t *dag, latnode_t *end)
{
    latlink_t *next;

    next = ps_lattice_popq(dag);
    if (next == NULL)
        return NULL;

    /* Decrease fanin count for destination node and expand outgoing
     * edges if all incoming edges have been seen. */
    --next->to->info.fanin;
    if (next->to->info.fanin == 0) {
        latlink_list_t *x;

        if (end == NULL) end = dag->end;
        if (next->to == end) {
            /* If we have traversed all links entering the end node,
             * clear the queue, causing future calls to this function
             * to return NULL. */
            ps_lattice_delq(dag);
            return next;
        }

        /* Extend all outgoing edges. */
        for (x = next->to->exits; x; x = x->next)
            ps_lattice_pushq(dag, x->link);
    }
    return next;
}

latlink_t *
ps_lattice_reverse_edges(ps_lattice_t *dag, latnode_t *start, latnode_t *end)
{
    latnode_t *node;
    latlink_list_t *x;

    /* Cancel any unfinished traversal. */
    ps_lattice_delq(dag);

    /* Initialize node fanout counts and path scores. */
    for (node = dag->nodes; node; node = node->next) {
        node->info.fanin = 0;
        for (x = node->exits; x; x = x->next)
            ++node->info.fanin;
    }

    /* Initialize agenda with all entries from end. */
    if (end == NULL) end = dag->end;
    for (x = end->entries; x; x = x->next)
        ps_lattice_pushq(dag, x->link);

    /* Pull the first edge off the queue. */
    return ps_lattice_reverse_next(dag, start);
}

latlink_t *
ps_lattice_reverse_next(ps_lattice_t *dag, latnode_t *start)
{
    latlink_t *next;

    next = ps_lattice_popq(dag);
    if (next == NULL)
        return NULL;

    /* Decrease fanout count for source node and expand incoming
     * edges if all incoming edges have been seen. */
    --next->from->info.fanin;
    if (next->from->info.fanin == 0) {
        latlink_list_t *x;

        if (start == NULL) start = dag->start;
        if (next->from == start) {
            /* If we have traversed all links entering the start node,
             * clear the queue, causing future calls to this function
             * to return NULL. */
            ps_lattice_delq(dag);
            return next;
        }

        /* Extend all outgoing edges. */
        for (x = next->from->entries; x; x = x->next)
            ps_lattice_pushq(dag, x->link);
    }
    return next;
}

/*
 * Find the best score from dag->start to end point of any link and
 * use it to update links further down the path.  This is like
 * single-source shortest path search, except that it is done over
 * edges rather than nodes, which allows us to do exact trigram scoring.
 *
 * Helpfully enough, we get half of the posterior probability
 * calculation for free that way too.  (interesting research topic: is
 * there a reliable Viterbi analogue to word-level Forward-Backward
 * like there is for state-level?  Or, is it just lattice density?)
 */
latlink_t *
ps_lattice_bestpath(ps_lattice_t *dag, ngram_model_t *lmset,
                    float32 lwf, float32 ascale)
{
    ps_search_t *search;
    latnode_t *node;
    latlink_t *link;
    latlink_t *bestend;
    latlink_list_t *x;
    logmath_t *lmath;
    int32 bestescr;

    search = dag->search;
    lmath = dag->search->acmod->lmath;

    /* Initialize path scores for all links exiting dag->start, and
     * set all other scores to the minimum.  Also initialize alphas to
     * log-zero. */
    for (node = dag->nodes; node; node = node->next) {
        for (x = node->exits; x; x = x->next) {
            x->link->path_scr = MAX_NEG_INT32;
            x->link->alpha = logmath_get_zero(lmath);
        }
    }
    for (x = dag->start->exits; x; x = x->next) {
        int32 n_used;

        /* Ignore filler words. */
        if (ISA_FILLER_WORD(search, x->link->to->basewid)
            && x->link->to != dag->end)
            continue;

        /* Best path points to dag->start, obviously. */
        if (lmset)
            x->link->path_scr = x->link->ascr +
                ngram_bg_score(lmset, x->link->to->basewid,
                               ps_search_start_wid(search), &n_used) * lwf;
        else
            x->link->path_scr = x->link->ascr;
        x->link->best_prev = NULL;
        /* Alpha is just the acoustic score as there are no predecessors. */
        x->link->alpha = x->link->ascr * ascale;
    }

    /* Traverse the edges in the graph, updating path scores. */
    for (link = ps_lattice_traverse_edges(dag, NULL, NULL);
         link; link = ps_lattice_traverse_next(dag, NULL)) {
        int32 bprob, n_used;

        /* Skip filler nodes in traversal. */
        if (ISA_FILLER_WORD(search, link->from->basewid) && link->from != dag->start)
            continue;
        if (ISA_FILLER_WORD(search, link->to->basewid) && link->to != dag->end)
            continue;

        /* Sanity check, we should not be traversing edges that
         * weren't previously updated, otherwise nasty overflows will result. */
        assert(link->path_scr != MAX_NEG_INT32);

        /* Calculate common bigram probability for all alphas. */
        if (lmset)
            bprob = ngram_ng_prob(lmset,
                                  link->to->basewid,
                                  &link->from->basewid, 1, &n_used);
        else
            bprob = 0;
        /* Update scores for all paths exiting link->to. */
        for (x = link->to->exits; x; x = x->next) {
            int32 tscore, score;

            /* Skip links to filler words in update. */
            if (ISA_FILLER_WORD(search, x->link->to->basewid)
                && x->link->to != dag->end)
                continue;

            /* Update alpha with sum of previous alphas. */
            score = link->alpha + bprob + x->link->ascr * ascale;
            x->link->alpha = logmath_add(lmath, x->link->alpha, score);

            /* Calculate trigram score for bestpath. */
            if (lmset)
                tscore = ngram_tg_score(lmset, x->link->to->basewid,
                                        link->to->basewid,
                                        link->from->basewid, &n_used) * lwf;
            else
                tscore = 0;
            /* Update link score with maximum link score. */
            score = link->path_scr + tscore + x->link->ascr;
            if (score > x->link->path_scr) {
                x->link->path_scr = score;
                x->link->best_prev = link;
            }
        }
    }

    /* Find best link entering final node, and calculate normalizer
     * for posterior probabilities. */
    bestend = NULL;
    bestescr = MAX_NEG_INT32;
    dag->norm = logmath_get_zero(lmath);

    for (x = dag->end->entries; x; x = x->next) {
        if (ISA_FILLER_WORD(search, x->link->from->basewid))
            continue;
        dag->norm = logmath_add(lmath, dag->norm, x->link->alpha);
        if (x->link->path_scr > bestescr) {
            bestescr = x->link->path_scr;
            bestend = x->link;
        }
    }

    return bestend;
}

int32
ps_lattice_posterior(ps_lattice_t *dag, ngram_model_t *lmset,
                     float32 ascale)
{
    ps_search_t *search;
    logmath_t *lmath;
    latnode_t *node;
    latlink_t *link;
    latlink_list_t *x;

    search = dag->search;
    lmath = dag->search->acmod->lmath;

    /* Reset all betas to zero. */
    for (node = dag->nodes; node; node = node->next) {
        for (x = node->exits; x; x = x->next) {
            x->link->beta = logmath_get_zero(lmath);
        }
    }

    /* Accumulate backward probabilities for all links. */
    for (link = ps_lattice_reverse_edges(dag, NULL, NULL);
         link; link = ps_lattice_reverse_next(dag, NULL)) {
        /* Skip filler nodes in traversal. */
        if (ISA_FILLER_WORD(search, link->from->basewid) && link->from != dag->start)
            continue;
        if (ISA_FILLER_WORD(search, link->to->basewid) && link->to != dag->end)
            continue;

        /* Beta for arcs into dag->end = 1.0. */
        if (link->to == dag->end)
            link->beta = 0;
        else {
            int32 bprob, n_used;

            /* Calculate LM probability. */
            if (lmset)
                bprob = ngram_ng_prob(lmset, link->to->basewid,
                                      &link->from->basewid, 1, &n_used);
            else
                bprob = 0;

            /* Update beta from all outgoing betas. */
            for (x = link->to->exits; x; x = x->next) {
                if (ISA_FILLER_WORD(search, x->link->to->basewid) && x->link->to != dag->end)
                    continue;
                link->beta = logmath_add(lmath, link->beta,
                                         x->link->beta + bprob + x->link->ascr * ascale);
            }
        }
    }
    return 0;
}


/* Parameters to prune n-best alternatives search */
#define MAX_PATHS	500     /* Max allowed active paths at any time */
#define MAX_HYP_TRIES	10000

/*
 * For each node in any path between from and end of utt, find the
 * best score from "from".sf to end of utt.  (NOTE: Uses bigram probs;
 * this is an estimate of the best score from "from".)  (NOTE #2: yes,
 * this is the "heuristic score" used in A* search)
 */
static int32
best_rem_score(ps_astar_t *nbest, latnode_t * from)
{
    ps_lattice_t *dag;
    latlink_list_t *x;
    int32 bestscore, score;

    dag = nbest->dag;
    if (from->info.rem_score <= 0)
        return (from->info.rem_score);

    /* Best score from "from" to end of utt not known; compute from successors */
    bestscore = WORST_SCORE;
    for (x = from->exits; x; x = x->next) {
        int32 n_used;

        score = best_rem_score(nbest, x->link->to);
        score += x->link->ascr;
        if (nbest->lmset)
            score += ngram_bg_score(nbest->lmset, x->link->to->basewid,
                                    from->basewid, &n_used) * nbest->lwf;
        if (score > bestscore)
            bestscore = score;
    }
    from->info.rem_score = bestscore;

    return bestscore;
}

/*
 * Insert newpath in sorted (by path score) list of paths.  But if newpath is
 * too far down the list, drop it (FIXME: necessary?)
 * total_score = path score (newpath) + rem_score to end of utt.
 */
static void
path_insert(ps_astar_t *nbest, latpath_t *newpath, int32 total_score)
{
    ps_lattice_t *dag;
    latpath_t *prev, *p;
    int32 i;

    dag = nbest->dag;
    prev = NULL;
    for (i = 0, p = nbest->path_list; (i < MAX_PATHS) && p; p = p->next, i++) {
        if ((p->score + p->node->info.rem_score) < total_score)
            break;
        prev = p;
    }

    /* newpath should be inserted between prev and p */
    if (i < MAX_PATHS) {
        /* Insert new partial hyp */
        newpath->next = p;
        if (!prev)
            nbest->path_list = newpath;
        else
            prev->next = newpath;
        if (!p)
            nbest->path_tail = newpath;

        nbest->n_path++;
        nbest->n_hyp_insert++;
        nbest->insert_depth += i;
    }
    else {
        /* newpath score too low; reject it and also prune paths beyond MAX_PATHS */
        nbest->path_tail = prev;
        prev->next = NULL;
        nbest->n_path = MAX_PATHS;
        listelem_free(nbest->latpath_alloc, newpath);

        nbest->n_hyp_reject++;
        for (; p; p = newpath) {
            newpath = p->next;
            listelem_free(nbest->latpath_alloc, p);
            nbest->n_hyp_reject++;
        }
    }
}

/* Find all possible extensions to given partial path */
static void
path_extend(ps_astar_t *nbest, latpath_t * path)
{
    latlink_list_t *x;
    latpath_t *newpath;
    int32 total_score, tail_score;
    ps_lattice_t *dag;

    dag = nbest->dag;

    /* Consider all successors of path->node */
    for (x = path->node->exits; x; x = x->next) {
        int32 n_used;

        /* Skip successor if no path from it reaches the final node */
        if (x->link->to->info.rem_score <= WORST_SCORE)
            continue;

        /* Create path extension and compute exact score for this extension */
        newpath = listelem_malloc(nbest->latpath_alloc);
        newpath->node = x->link->to;
        newpath->parent = path;
        newpath->score = path->score + x->link->ascr;
        if (nbest->lmset) {
            if (path->parent) {
                newpath->score += nbest->lwf
                    * ngram_tg_score(nbest->lmset, newpath->node->basewid,
                                     path->node->basewid,
                                     path->parent->node->basewid, &n_used);
            }
            else 
                newpath->score += nbest->lwf
                    * ngram_bg_score(nbest->lmset, newpath->node->basewid,
                                     path->node->basewid, &n_used);
        }

        /* Insert new partial path hypothesis into sorted path_list */
        nbest->n_hyp_tried++;
        total_score = newpath->score + newpath->node->info.rem_score;

        /* First see if hyp would be worse than the worst */
        if (nbest->n_path >= MAX_PATHS) {
            tail_score =
                nbest->path_tail->score
                + nbest->path_tail->node->info.rem_score;
            if (total_score < tail_score) {
                listelem_free(nbest->latpath_alloc, newpath);
                nbest->n_hyp_reject++;
                continue;
            }
        }

        path_insert(nbest, newpath, total_score);
    }
}

ps_astar_t *
ps_astar_start(ps_lattice_t *dag,
                  ngram_model_t *lmset,
                  float32 lwf,
                  int sf, int ef,
                  int w1, int w2)
{
    ps_astar_t *nbest;
    latnode_t *node;

    nbest = ckd_calloc(1, sizeof(*nbest));
    nbest->dag = dag;
    nbest->lmset = lmset;
    nbest->lwf = lwf;
    nbest->sf = sf;
    if (ef < 0)
        nbest->ef = dag->n_frames - ef;
    else
        nbest->ef = ef;
    nbest->w1 = w1;
    nbest->w2 = w2;
    nbest->latpath_alloc = listelem_alloc_init(sizeof(latpath_t));

    /* Initialize rem_score (A* heuristic) to default values */
    for (node = dag->nodes; node; node = node->next) {
        if (node == dag->end)
            node->info.rem_score = 0;
        else if (node->exits == NULL)
            node->info.rem_score = WORST_SCORE;
        else
            node->info.rem_score = 1;   /* +ve => unknown value */
    }

    /* Create initial partial hypotheses list consisting of nodes starting at sf */
    nbest->path_list = nbest->path_tail = NULL;
    for (node = dag->nodes; node; node = node->next) {
        if (node->sf == sf) {
            latpath_t *path;
            int32 n_used;

            best_rem_score(nbest, node);
            path = listelem_malloc(nbest->latpath_alloc);
            path->node = node;
            path->parent = NULL;
            if (nbest->lmset)
                path->score = nbest->lwf *
                    (w1 < 0)
                    ? ngram_bg_score(nbest->lmset, node->basewid, w2, &n_used)
                    : ngram_tg_score(nbest->lmset, node->basewid, w2, w1, &n_used);
            else
                path->score = 0;
            path_insert(nbest, path, path->score + node->info.rem_score);
        }
    }

    return nbest;
}

latpath_t *
ps_astar_next(ps_astar_t *nbest)
{
    latpath_t *top;
    ps_lattice_t *dag;

    dag = nbest->dag;

    /* Pop the top (best) partial hypothesis */
    while ((top = nbest->path_list) != NULL) {
        nbest->path_list = nbest->path_list->next;
        if (top == nbest->path_tail)
            nbest->path_tail = NULL;
        nbest->n_path--;

        /* Complete hypothesis? */
        if ((top->node->sf >= nbest->ef)
            || ((top->node == dag->end) &&
                (nbest->ef > dag->end->sf))) {
            /* FIXME: Verify that it is non-empty. */
            return top;
        }
        else {
            if (top->node->fef < nbest->ef)
                path_extend(nbest, top);
        }

        /*
         * Add top to paths already processed; cannot be freed because other paths
         * point to it.
         */
        top->next = nbest->paths_done;
        nbest->paths_done = top;
    }

    /* Did not find any more paths to extend. */
    return NULL;
}

char const *
ps_astar_hyp(ps_astar_t *nbest, latpath_t *path)
{
    ps_search_t *search;
    latpath_t *p;
    size_t len;
    char *c;
    char *hyp;

    search = nbest->dag->search;

    /* Backtrace once to get hypothesis length. */
    len = 0;
    for (p = path; p; p = p->parent) {
        if (ISA_REAL_WORD(search, p->node->basewid))
            len += strlen(dict_word_str(ps_search_dict(search), p->node->basewid)) + 1;
    }

    /* Backtrace again to construct hypothesis string. */
    hyp = ckd_calloc(1, len);
    c = hyp + len - 1;
    for (p = path; p; p = p->parent) {
        if (ISA_REAL_WORD(search, p->node->basewid)) {
            len = strlen(dict_word_str(ps_search_dict(search), p->node->basewid));
            c -= len;
            memcpy(c, dict_word_str(ps_search_dict(search), p->node->basewid), len);
            if (c > hyp) {
                --c;
                *c = ' ';
            }
        }
    }

    nbest->hyps = glist_add_ptr(nbest->hyps, hyp);
    return hyp;
}

void
ps_astar_finish(ps_astar_t *nbest)
{
    gnode_t *gn;

    /* Free all hyps. */
    for (gn = nbest->hyps; gn; gn = gnode_next(gn)) {
        ckd_free(gnode_ptr(gn));
    }
    glist_free(nbest->hyps);
    /* Free all paths. */
    listelem_alloc_free(nbest->latpath_alloc);
    /* Free the Henge. */
    ckd_free(nbest);
}
