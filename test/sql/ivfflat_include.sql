-- IVFFlat INCLUDE column chains (see docs/filtered-ivf.md)

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
SET ivfflat.prefilter = off;
SET ivfflat.include_filter = on;
SET ivfflat.iterative_scan = off;

CREATE FUNCTION include_matches(q text) RETURNS boolean AS $$
DECLARE
	a text;
	b text;
BEGIN
	PERFORM set_config('enable_indexscan', 'on', true);
	PERFORM set_config('enable_seqscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('enable_sort', 'off', true);
	EXECUTE 'SELECT coalesce(string_agg(id::text, '','' ORDER BY id), '''') FROM (' || q || ') s' INTO a;
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_seqscan', 'on', true);
	EXECUTE 'SELECT coalesce(string_agg(id::text, '','' ORDER BY id), '''') FROM (' || q || ') s' INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION include_startup(q text) RETURNS numeric AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN ' || q LOOP
		IF ln LIKE '%IvfIncludeScan%' THEN
			RETURN substring(ln from 'cost=([0-9]+\.[0-9]+)')::numeric;
		END IF;
	END LOOP;
	RETURN NULL;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION explain_include(q text) RETURNS SETOF text AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF ln LIKE '%Include%' THEN
			RETURN NEXT ltrim(ln, ' ->');
		END IF;
	END LOOP;
END;
$$ LANGUAGE plpgsql;

-- type and width checks
CREATE TABLE inc_types (v vector(3), s varchar(512), c int);
INSERT INTO inc_types VALUES ('[1,2,3]', 'ok', 1);
CREATE INDEX ON inc_types USING ivfflat (v vector_l2_ops) INCLUDE (s) WITH (lists = 1);

CREATE TABLE inc_wide (v vector(3), s varchar(512));
INSERT INTO inc_wide VALUES ('[1,2,3]', repeat('x', 508));
CREATE INDEX ON inc_wide USING ivfflat (v vector_l2_ops) INCLUDE (s) WITH (lists = 1);
INSERT INTO inc_wide VALUES ('[4,5,6]', repeat('x', 512));

CREATE TABLE inc_build_wide (v vector(3), s varchar(512));
INSERT INTO inc_build_wide VALUES ('[1,2,3]', repeat('x', 512));
CREATE INDEX ON inc_build_wide USING ivfflat (v vector_l2_ops) INCLUDE (s) WITH (lists = 1);

CREATE TABLE inc_varchar513 (v vector(3), s varchar(513));
CREATE INDEX ON inc_varchar513 USING ivfflat (v vector_l2_ops) INCLUDE (s) WITH (lists = 1);

CREATE TABLE inc_text (v vector(3), s text);
CREATE INDEX ON inc_text USING ivfflat (v vector_l2_ops) INCLUDE (s) WITH (lists = 1);

CREATE TABLE inc_twokey (v vector(3), c int);
CREATE INDEX ON inc_twokey USING ivfflat (v vector_l2_ops, c) WITH (lists = 1);

-- list item fit: 14 included int4 columns fit next to vector(2000); 15 do not
CREATE TABLE inc_fit14 (v vector(2000),
	c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int,
	c8 int, c9 int, c10 int, c11 int, c12 int, c13 int, c14 int);
CREATE INDEX ON inc_fit14 USING ivfflat (v vector_l2_ops)
	INCLUDE (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14)
	WITH (lists = 1);

CREATE TABLE inc_fit15 (v vector(2000),
	c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int,
	c8 int, c9 int, c10 int, c11 int, c12 int, c13 int, c14 int, c15 int);
CREATE INDEX ON inc_fit15 USING ivfflat (v vector_l2_ops)
	INCLUDE (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15)
	WITH (lists = 1);

-- vector(128) with INDEX_MAX_KEYS - 1 included columns; one more fails in the parser
DO $$
DECLARE
	defs text;
	cols text;
BEGIN
	SELECT string_agg('c' || i || ' int', ', '), string_agg('c' || i, ', ')
	INTO defs, cols
	FROM generate_series(1, 31) i;
	EXECUTE 'CREATE TABLE inc_wide31 (v vector(128), ' || defs || ')';
	EXECUTE 'CREATE INDEX ON inc_wide31 USING ivfflat (v vector_l2_ops) INCLUDE (' || cols || ') WITH (lists = 1)';
END $$;

DO $$
DECLARE
	defs text;
	cols text;
BEGIN
	SELECT string_agg('c' || i || ' int', ', '), string_agg('c' || i, ', ')
	INTO defs, cols
	FROM generate_series(1, 32) i;
	EXECUTE 'CREATE TABLE inc_wide32 (v vector(128), ' || defs || ')';
	EXECUTE 'CREATE INDEX ON inc_wide32 USING ivfflat (v vector_l2_ops) INCLUDE (' || cols || ') WITH (lists = 1)';
END $$;

-- vector(2000) plus one included column builds and scans
CREATE TABLE inc_dim (id int, v vector(2000));
INSERT INTO inc_dim VALUES (1, array_fill(1, ARRAY[2000])::vector), (2, array_fill(2, ARRAY[2000])::vector);
CREATE INDEX ON inc_dim USING ivfflat (v vector_l2_ops) INCLUDE (id) WITH (lists = 1);
SELECT include_matches($$SELECT id FROM inc_dim WHERE id = 2 ORDER BY v <-> array_fill(2, ARRAY[2000])::vector LIMIT 1$$);

-- equality, range, IN, IS NULL, AND, OR, nested AND/OR, and clauses with a non-included column
CREATE TABLE inc_t (id int, category int, price int, note text, embedding vector(3));
INSERT INTO inc_t
SELECT i,
	CASE WHEN i % 17 = 0 THEN NULL ELSE i % 10 END,
	i % 100,
	CASE WHEN i % 5 = 0 THEN 'keep' ELSE 'drop' END,
	ARRAY[i, i, i]::vector
FROM generate_series(1, 2000) i;
CREATE INDEX inc_t_ivf ON inc_t USING ivfflat (embedding vector_l2_ops) INCLUDE (category, price) WITH (lists = 1);
ANALYZE inc_t;

SELECT include_matches($$SELECT id FROM inc_t WHERE category = 1 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category BETWEEN 2 AND 3 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category IN (1, 3) ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE 5 > category ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category IS NULL ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category = 1 AND price < 10 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category = 1 OR price < 10 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category = 1 OR category = 2 OR price < 5 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE price < 50 AND (category = 1 OR price < 10) ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE category = 1 OR note = 'keep' ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE (category = 1 AND price < 50) OR price > 95 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE (category = 1 AND note = 'keep') OR price < 10 ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE (category = 1 OR (price < 10 AND category IS NULL)) AND (price > 5 OR category = 2) ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE (category IN (1, 2) AND price < 30) OR (category = 3 AND price > 70) ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
SELECT include_matches($$SELECT id FROM inc_t WHERE note = 'keep' ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);

-- one included column is pushed; the other chain is not named
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE category = 1 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
-- two included columns (AND)
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE category = 1 AND price < 10 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
-- two included columns in one OR are pushed
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE category = 1 OR price < 10 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
-- AND inside OR, and OR inside AND, are pushed as a tree
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE (category = 1 AND price < 50) OR price > 95 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE (category = 1 OR (price < 10 AND category IS NULL)) AND (price > 5 OR category = 2) ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
-- an AND arm on a non-included column is left out of the pushed filter
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE (category = 1 AND note = 'keep') OR price < 10 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
-- an OR mixing an included column and a non-included column stays on the child Filter
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE category = 1 OR note = 'keep' ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
-- a column that is not included stays on Filter
EXPLAIN (COSTS OFF) SELECT id FROM inc_t WHERE note = 'keep' ORDER BY embedding <-> '[0,0,0]' LIMIT 5;

-- an explicit collation is pushed: the leaf runs the clause on the stored value
CREATE TABLE inc_coll (id int, label varchar(8), embedding vector(3));
INSERT INTO inc_coll SELECT i, chr(65 + i % 26) || chr(97 + i % 7), ARRAY[i, i, i]::vector FROM generate_series(1, 1000) i;
CREATE INDEX ON inc_coll USING ivfflat (embedding vector_l2_ops) INCLUDE (label) WITH (lists = 1);
ANALYZE inc_coll;
SELECT include_matches($$SELECT id FROM inc_coll WHERE label < 'Bb' COLLATE "C" ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
EXPLAIN (COSTS OFF) SELECT id FROM inc_coll WHERE label = 'Cd' COLLATE "C" ORDER BY embedding <-> '[0,0,0]' LIMIT 5;

-- row-level security: a clause that is not leakproof waits for the policy
CREATE TYPE inc_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE inc_rls (id int, owner int, mood inc_mood, embedding vector(3));
INSERT INTO inc_rls SELECT i, i % 10, (ARRAY['sad', 'ok', 'happy'])[1 + i % 3]::inc_mood, ARRAY[i, i, i]::vector FROM generate_series(1, 1000) i;
CREATE INDEX ON inc_rls USING ivfflat (embedding vector_l2_ops) INCLUDE (owner, mood) WITH (lists = 1);
ANALYZE inc_rls;
ALTER TABLE inc_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY inc_rls_owner ON inc_rls USING (owner = 1);
CREATE ROLE regress_ivf_rls;
GRANT SELECT ON inc_rls TO regress_ivf_rls;
SET ROLE regress_ivf_rls;
EXPLAIN (COSTS OFF) SELECT id FROM inc_rls WHERE mood = 'happy' ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
SELECT include_matches($$SELECT id FROM inc_rls WHERE mood = 'happy' ORDER BY embedding <-> '[0,0,0]' LIMIT 8$$);
RESET ROLE;
-- the table owner bypasses the policy, so the same clause is pushed
EXPLAIN (COSTS OFF) SELECT id FROM inc_rls WHERE mood = 'happy' ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
DROP TABLE inc_rls;
DROP TYPE inc_mood;
DROP ROLE regress_ivf_rls;

-- an included-column update is visible, and the old value is not
UPDATE inc_t SET category = 42 WHERE id = 1;
SELECT id FROM inc_t WHERE category = 42 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
SELECT count(*) FROM inc_t WHERE id = 1 AND category = 1;

-- vacuum, then an insert into the hole, still matches a sequential scan
DELETE FROM inc_t WHERE id % 2 = 0;
VACUUM inc_t;
SELECT include_matches($$SELECT id FROM inc_t WHERE category = 3 ORDER BY embedding <-> '[0,0,0]' LIMIT 15$$);
INSERT INTO inc_t SELECT 5000 + i, 3, 1, 'keep', ARRAY[i, i, i]::vector FROM generate_series(1, 30) i;
SELECT include_matches($$SELECT id FROM inc_t WHERE category = 3 ORDER BY embedding <-> '[0,0,0]' LIMIT 15$$);

-- unfiltered order on a version-2 index matches a version-1 index at lists = 1
CREATE TABLE inc_plain (id int, embedding vector(3));
INSERT INTO inc_plain SELECT id, embedding FROM inc_t;
CREATE INDEX inc_plain_v2 ON inc_plain USING ivfflat (embedding vector_l2_ops) INCLUDE (id) WITH (lists = 1);
SELECT id AS id INTO TEMP inc_v2_ids FROM inc_plain ORDER BY embedding <-> '[5,5,5]' LIMIT 10;
DROP INDEX inc_plain_v2;
CREATE INDEX inc_plain_v1 ON inc_plain USING ivfflat (embedding vector_l2_ops) WITH (lists = 1);
SELECT id AS id INTO TEMP inc_v1_ids FROM inc_plain ORDER BY embedding <-> '[5,5,5]' LIMIT 10;
SELECT (SELECT count(*) FROM (TABLE inc_v2_ids EXCEPT TABLE inc_v1_ids) d) = 0
	AND (SELECT count(*) FROM (TABLE inc_v1_ids EXCEPT TABLE inc_v2_ids) d) = 0 AS unfiltered_match;

-- include_mem below the bitmap skips the path; a cached plan falls back at runtime
CREATE TABLE inc_mem (id int, category int, embedding vector(3));
INSERT INTO inc_mem SELECT i, i % 10, ARRAY[i, i, i]::vector FROM generate_series(1, 400) i;
CREATE INDEX ON inc_mem USING ivfflat (embedding vector_l2_ops) INCLUDE (category) WITH (lists = 1);
ANALYZE inc_mem;
SET ivfflat.include_mem = 0;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mem WHERE category = 1 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
SELECT include_matches($$SELECT id FROM inc_mem WHERE category = 1 ORDER BY embedding <-> '[0,0,0]' LIMIT 5$$);
SET ivfflat.include_mem = -1;
SET plan_cache_mode = force_generic_plan;
PREPARE inc_q AS SELECT id FROM inc_mem WHERE category = 1 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
SELECT explain_include($$EXECUTE inc_q$$);
SET ivfflat.include_mem = 0;
SELECT explain_include($$EXECUTE inc_q$$);
CREATE TEMP TABLE inc_fb AS EXECUTE inc_q;
SET enable_indexscan = off;
SET enable_seqscan = on;
CREATE TEMP TABLE inc_seq AS
	SELECT id FROM inc_mem WHERE category = 1 ORDER BY embedding <-> '[0,0,0]' LIMIT 5;
SELECT (SELECT count(*) FROM (TABLE inc_fb EXCEPT TABLE inc_seq) d) = 0
	AND (SELECT count(*) FROM (TABLE inc_seq EXCEPT TABLE inc_fb) d) = 0 AS fallback_matches;
SET enable_indexscan = on;
SET enable_seqscan = off;
DEALLOCATE inc_q;
SET plan_cache_mode = auto;
SET ivfflat.include_mem = -1;

-- a btree on a column that is not included still uses IvfPrefilterScan
CREATE TABLE inc_mix (id int, category int, extra int, embedding vector(4));
INSERT INTO inc_mix SELECT i, i % 10, i % 7, ARRAY[i, i, i, i]::vector FROM generate_series(1, 800) i;
CREATE INDEX inc_mix_ivf ON inc_mix USING ivfflat (embedding vector_l2_ops) INCLUDE (category) WITH (lists = 1);
CREATE INDEX inc_mix_extra ON inc_mix (extra);
CREATE INDEX inc_mix_cat ON inc_mix (category);
ANALYZE inc_mix;
SET ivfflat.prefilter = on;
SET ivfflat.include_filter = on;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mix WHERE extra = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5;
SET ivfflat.include_filter = off;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mix WHERE extra = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5;
SELECT include_matches($$SELECT id FROM inc_mix WHERE extra = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5$$);

-- included column only: IvfIncludeScan, no Prefilter Index
SET ivfflat.prefilter = off;
SET ivfflat.include_filter = on;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mix WHERE category = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5;

-- both matches: one custom scan, and turning a GUC off switches it
SET ivfflat.prefilter = on;
SET ivfflat.include_filter = on;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mix WHERE category = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5;
SET ivfflat.prefilter = off;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mix WHERE category = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5;
SET ivfflat.prefilter = on;
SET ivfflat.include_filter = off;
EXPLAIN (COSTS OFF) SELECT id FROM inc_mix WHERE category = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5;
SELECT include_matches($$SELECT id FROM inc_mix WHERE category = 1 ORDER BY embedding <-> '[0,0,0,0]' LIMIT 5$$);

-- cost shape: startup stays on one batch until a LIMIT needs more lists
CREATE TABLE inc_cost (id int, category int, embedding vector(64));
INSERT INTO inc_cost
SELECT i, i % 50,
	(('[' || (SELECT string_agg(((i * 17 + g) % 97)::text, ',') FROM generate_series(1, 64) g) || ']')::vector)
FROM generate_series(1, 10000) i;
CREATE INDEX ON inc_cost USING ivfflat (embedding vector_l2_ops) INCLUDE (category) WITH (lists = 100);
ANALYZE inc_cost;
SET ivfflat.prefilter = off;
SET ivfflat.include_filter = on;
SET ivfflat.probes = 10;
SET ivfflat.max_probes = 100;
SET ivfflat.iterative_scan = off;

CREATE FUNCTION plan_text(q text) RETURNS text AS $$
DECLARE
	ln text;
	buf text := '';
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN ' || q LOOP
		buf := buf || ln || E'\n';
	END LOOP;
	RETURN buf;
END;
$$ LANGUAGE plpgsql;

SELECT include_startup($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 5$$)
	= include_startup($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 100$$)
	AS off_limit_same_startup;

SET ivfflat.iterative_scan = relaxed_order;
SELECT include_startup($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 5$$)
	< include_startup($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 100$$)
	AS relaxed_limit_raises_startup;
SELECT position('IvfIncludeScan' in plan_text($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 100$$)) > 0
	AND position('Sort' in plan_text($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 100$$)) = 0
	AS no_sort_under_limit;

SET ivfflat.max_probes = 10;
SELECT include_startup($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 5$$)
	= include_startup($$SELECT id FROM inc_cost WHERE category = 1 ORDER BY embedding <-> (SELECT array_fill(0.0, ARRAY[64])::real[]::vector) LIMIT 100$$)
	AS max_probes_caps_startup;
