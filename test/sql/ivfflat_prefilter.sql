-- IVF prefilter scan (see src/ivfprefilter.c)

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;

-- returns only version-stable lines from EXPLAIN ANALYZE
CREATE FUNCTION explain_prefilter(query text) RETURNS SETOF text AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || query LOOP
		IF ln LIKE '%Prefilter%' THEN
			RETURN NEXT ltrim(ln, ' ->');
		END IF;
	END LOOP;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE t (id int4, category int4, extra int4, val vector(3));
INSERT INTO t SELECT i, i % 10, i % 7, ARRAY[i, i, i]::vector FROM generate_series(1, 1000) i;
INSERT INTO t SELECT 1000 + i, NULL, 0, ARRAY[2000 + i, 2000 + i, 2000 + i]::vector FROM generate_series(1, 5) i;
CREATE INDEX t_val_idx ON t USING ivfflat (val vector_l2_ops) WITH (lists = 1);
CREATE INDEX t_category_idx ON t (category);
ANALYZE t;

-- plan shape
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;

-- results (identical with prefilter on and off)
SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;
SET ivfflat.prefilter = off;
SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;
SET ivfflat.prefilter = on;

-- operators
SELECT id FROM t WHERE category BETWEEN 2 AND 3 ORDER BY val <-> '[0,0,0]' LIMIT 5;
SELECT id FROM t WHERE category IN (1, 3) ORDER BY val <-> '[0,0,0]' LIMIT 5;
SELECT id FROM t WHERE 5 > category ORDER BY val <-> '[0,0,0]' LIMIT 5;
SELECT id FROM t WHERE category IS NULL ORDER BY val <-> '[0,0,0]' LIMIT 5;
SELECT explain_prefilter($$SELECT id FROM t WHERE 5 > category ORDER BY val <-> '[0,0,0]' LIMIT 5$$);
SELECT explain_prefilter($$SELECT id FROM t WHERE category IS NULL ORDER BY val <-> '[0,0,0]' LIMIT 5$$);

-- <> is not a B-tree strategy: no prefilter
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE category <> 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;

-- OR expression is not a single B-tree scan key: no prefilter
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE category = 1 OR category = 2 ORDER BY val <-> '[0,0,0]' LIMIT 5;

-- volatile expressions are not matched: no prefilter
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE category = (random() * 0)::int4 ORDER BY val <-> '[0,0,0]' LIMIT 5;

-- no WHERE clause: no prefilter
EXPLAIN (COSTS OFF) SELECT id FROM t ORDER BY val <-> '[0,0,0]' LIMIT 5;

-- unindexed column: no prefilter
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 5;

-- unmatched clauses stay on the child Filter
SELECT explain_prefilter($$SELECT id FROM t WHERE category = 1 AND extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 3$$);
SELECT id FROM t WHERE category = 1 AND extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 3;

-- multicolumn B-tree
CREATE INDEX t_multi_idx ON t (category, extra);
SELECT explain_prefilter($$SELECT id FROM t WHERE category = 1 AND extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 3$$);
-- prefix rule: no clause on column 0 of any usable index
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 3;
DROP INDEX t_multi_idx;

-- partial B-tree (predOK)
DROP INDEX t_category_idx;
CREATE INDEX t_partial_idx ON t (extra) WHERE category = 1;
SELECT explain_prefilter($$SELECT id FROM t WHERE category = 1 AND extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 3$$);
SELECT id FROM t WHERE category = 1 AND extra = 4 ORDER BY val <-> '[0,0,0]' LIMIT 3;
DROP INDEX t_partial_idx;

-- expression B-tree
CREATE INDEX t_expr_idx ON t (((category + 10)));
SELECT explain_prefilter($$SELECT id FROM t WHERE category + 10 = 11 ORDER BY val <-> '[0,0,0]' LIMIT 3$$);
SELECT id FROM t WHERE category + 10 = 11 ORDER BY val <-> '[0,0,0]' LIMIT 3;
DROP INDEX t_expr_idx;
CREATE INDEX t_category_idx ON t (category);

-- kill switch
SET ivfflat.prefilter = off;
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;
SET ivfflat.prefilter = on;

-- plan-time memory threshold
SET ivfflat.prefilter_mem = 1;
EXPLAIN (COSTS OFF) SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;
RESET ivfflat.prefilter_mem;

-- run-time memory fallback (cached plan, budget lowered after planning)
SET plan_cache_mode = force_generic_plan;
PREPARE qmem AS SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 3;
SELECT explain_prefilter('EXECUTE qmem');
SET ivfflat.prefilter_mem = 1;
SELECT explain_prefilter('EXECUTE qmem');
EXECUTE qmem;
RESET ivfflat.prefilter_mem;
DEALLOCATE qmem;

-- prepared statement with a parameter (runtime keys)
PREPARE qparam (int4) AS SELECT id FROM t WHERE category = $1 ORDER BY val <-> '[0,0,0]' LIMIT 3;
EXECUTE qparam(1);
EXECUTE qparam(2);
SELECT explain_prefilter('EXECUTE qparam(3)');
DEALLOCATE qparam;
RESET plan_cache_mode;

-- plan invalidation when the B-tree is dropped
SET plan_cache_mode = force_generic_plan;
PREPARE qdrop AS SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 3;
SELECT explain_prefilter('EXECUTE qdrop');
DROP INDEX t_category_idx;
EXECUTE qdrop;
SELECT explain_prefilter('EXECUTE qdrop');
DEALLOCATE qdrop;
RESET plan_cache_mode;
CREATE INDEX t_category_idx ON t (category);

-- correlated subplan: one rebuild per changed parameter
SELECT explain_prefilter($$SELECT (SELECT id FROM t WHERE category = v.c ORDER BY val <-> '[0,0,0]' LIMIT 1) FROM (VALUES (1), (2), (3)) v(c)$$);

-- cursors
BEGIN;
DECLARE cur SCROLL CURSOR FOR SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]';
FETCH 3 FROM cur;
FETCH BACKWARD 2 FROM cur;
COMMIT;

-- row locking
SELECT id FROM t WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 3 FOR UPDATE;

-- collated text column
CREATE TABLE t2 (id int4, name text, val vector(3));
INSERT INTO t2 SELECT i, 'name' || (i % 10), ARRAY[i, i, i]::vector FROM generate_series(1, 100) i;
CREATE INDEX t2_val_idx ON t2 USING ivfflat (val vector_l2_ops) WITH (lists = 1);
CREATE INDEX t2_name_idx ON t2 (name);
ANALYZE t2;
SELECT explain_prefilter($$SELECT id FROM t2 WHERE name = 'name1' ORDER BY val <-> '[0,0,0]' LIMIT 3$$);
SELECT id FROM t2 WHERE name = 'name1' ORDER BY val <-> '[0,0,0]' LIMIT 3;
DROP TABLE t2;

-- row-level security: a clause that is not leakproof is not a B-tree key before the policy
CREATE TYPE t3_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE t3 (id int4, owner int4, mood t3_mood, val vector(3));
INSERT INTO t3 SELECT i, i % 10, (ARRAY['sad', 'ok', 'happy'])[1 + i % 3]::t3_mood, ARRAY[i, i, i]::vector FROM generate_series(1, 1000) i;
CREATE INDEX t3_val_idx ON t3 USING ivfflat (val vector_l2_ops) WITH (lists = 1);
CREATE INDEX t3_mood_idx ON t3 (mood);
ANALYZE t3;
ALTER TABLE t3 ENABLE ROW LEVEL SECURITY;
CREATE POLICY t3_owner ON t3 USING (owner = 1);
CREATE ROLE regress_ivf_prefilter_rls;
GRANT SELECT ON t3 TO regress_ivf_prefilter_rls;
SET ROLE regress_ivf_prefilter_rls;
EXPLAIN (COSTS OFF) SELECT id FROM t3 WHERE mood = 'happy' ORDER BY val <-> '[0,0,0]' LIMIT 3;
SELECT id FROM t3 WHERE mood = 'happy' ORDER BY val <-> '[0,0,0]' LIMIT 3;
RESET ROLE;
-- the table owner bypasses the policy, so the B-tree prefilter is used
EXPLAIN (COSTS OFF) SELECT id FROM t3 WHERE mood = 'happy' ORDER BY val <-> '[0,0,0]' LIMIT 3;
DROP TABLE t3;
DROP TYPE t3_mood;
DROP ROLE regress_ivf_prefilter_rls;

-- partitioned table: one prefilter scan per leaf
CREATE TABLE pt (id int4, category int4, val vector(3)) PARTITION BY RANGE (id);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (1) TO (500);
CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (500) TO (1001);
INSERT INTO pt SELECT i, i % 10, ARRAY[i, i, i]::vector FROM generate_series(1, 1000) i;
CREATE INDEX ON pt USING ivfflat (val vector_l2_ops) WITH (lists = 1);
CREATE INDEX ON pt (category);
ANALYZE pt;
SELECT explain_prefilter($$SELECT id FROM pt WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5$$);
SELECT id FROM pt WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 5;
DROP TABLE pt;

-- iterative scan interplay (results identical with prefilter on and off)
CREATE TABLE t3 (id int4, category int4, val vector(3));
INSERT INTO t3 SELECT i, i % 10, ARRAY[i, i, i]::vector FROM generate_series(1, 1000) i;
CREATE INDEX t3_val_idx ON t3 USING ivfflat (val vector_l2_ops) WITH (lists = 3);
CREATE INDEX t3_category_idx ON t3 (category);
ANALYZE t3;
SET ivfflat.iterative_scan = relaxed_order;
SET ivfflat.max_probes = 3;
SELECT count(*) FROM (SELECT id FROM t3 WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 50) s;
CREATE TEMP TABLE res_on AS SELECT id FROM t3 WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 50;
SET ivfflat.prefilter = off;
CREATE TEMP TABLE res_off AS SELECT id FROM t3 WHERE category = 1 ORDER BY val <-> '[0,0,0]' LIMIT 50;
SET ivfflat.prefilter = on;
SELECT count(*) FROM (SELECT id FROM res_on EXCEPT SELECT id FROM res_off) s;
SELECT count(*) FROM (SELECT id FROM res_off EXCEPT SELECT id FROM res_on) s;
RESET ivfflat.iterative_scan;
RESET ivfflat.max_probes;
DROP TABLE t3;

-- options
SHOW ivfflat.prefilter;
SET ivfflat.prefilter_mem = -2;
RESET ivfflat.prefilter_mem;

DROP TABLE t;
DROP FUNCTION explain_prefilter(text);

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;
