use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

# Included-column chains vs the same probe count with the filter disabled.
# probes = 1 can miss neighbors. relaxed_order stops once LIMIT rows match,
# so a higher max_probes raises recall without becoming exact. Probing every
# list does. The same probe count is faster when included columns skip vectors.

my $dim = 64;
my $rows = 80000;
my $lists = 100;
my $limit = 20;
my $nqueries = 8;
my $array_sql = join(",", ('random()') x $dim);

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

$node->safe_psql("postgres", "CREATE EXTENSION vector;");
$node->safe_psql("postgres", "CREATE TABLE tst (i int4, category int4, v vector($dim));");
$node->safe_psql("postgres",
	"INSERT INTO tst SELECT i, i % 1000, ARRAY[$array_sql] FROM generate_series(1, $rows) i;"
);
$node->safe_psql("postgres",
	"CREATE INDEX idx ON tst USING ivfflat (v vector_l2_ops) INCLUDE (category) WITH (lists = $lists);"
);
$node->safe_psql("postgres", "ANALYZE tst;");

my @queries = ();
for (1 .. $nqueries)
{
	my @r = map { rand() } (1 .. $dim);
	push(@queries, "[" . join(",", @r) . "]");
}

sub exact_ids
{
	my ($query) = @_;
	# enable_indexscan does not disable IvfIncludeScan. Turn the custom
	# plans off so this distance cutoff is the sequential-scan result.
	my $res = $node->safe_psql("postgres", qq(
		SET enable_indexscan = off;
		SET enable_bitmapscan = off;
		SET ivfflat.include_filter = off;
		SET ivfflat.prefilter = off;
		WITH top AS (
			SELECT v <-> '$query' AS distance
			FROM tst
			WHERE category = 0
			ORDER BY distance
			LIMIT $limit
		)
		SELECT i FROM tst
		WHERE category = 0
		  AND (v <-> '$query') <= (SELECT MAX(distance) FROM top);
	));
	return split("\n", $res);
}

sub scan_ids
{
	my ($query, $probes, $max_probes, $iter, $include) = @_;
	my $res = $node->safe_psql("postgres", qq(
		SET enable_seqscan = off;
		SET enable_bitmapscan = off;
		SET enable_sort = off;
		SET ivfflat.prefilter = off;
		SET ivfflat.include_filter = $include;
		SET ivfflat.probes = $probes;
		SET ivfflat.max_probes = $max_probes;
		SET ivfflat.iterative_scan = $iter;
		SELECT i FROM tst WHERE category = 0 ORDER BY v <-> '$query' LIMIT $limit;
	));
	return split("\n", $res);
}

sub recall
{
	my ($probes, $max_probes, $iter, $include) = @_;
	my $correct = 0;
	my $total = 0;

	foreach my $query (@queries)
	{
		my %expected = map { $_ => 1 } exact_ids($query);
		my @actual = scan_ids($query, $probes, $max_probes, $iter, $include);

		foreach (@actual)
		{
			$correct++ if exists $expected{$_};
		}
		$total += $limit;
	}

	return $correct / $total;
}

sub batch_seconds
{
	my ($probes, $max_probes, $iter, $include) = @_;
	my $sql = qq(
		SET enable_seqscan = off;
		SET enable_bitmapscan = off;
		SET enable_sort = off;
		SET ivfflat.prefilter = off;
		SET ivfflat.include_filter = $include;
		SET ivfflat.probes = $probes;
		SET ivfflat.max_probes = $max_probes;
		SET ivfflat.iterative_scan = $iter;
	);

	foreach my $query (@queries)
	{
		$sql .= "SELECT i FROM tst WHERE category = 0 ORDER BY v <-> '$query' LIMIT $limit;\n";
	}

	# One pass warms the cache. Time the second.
	$node->safe_psql("postgres", $sql);
	my $t0 = time();
	$node->safe_psql("postgres", $sql);
	return time() - $t0;
}

my $low = recall(1, 1, 'off', 'on');
# relaxed_order stops once LIMIT rows match, so max_probes raises recall
# without scanning every list.
my $relaxed = recall(1, $lists, 'relaxed_order', 'on');
my $full = recall($lists, $lists, 'off', 'on');

diag("recall probes=1 $low  relaxed max_probes=$lists $relaxed  probes=$lists $full");
cmp_ok($low, '<', 0.50, 'probes=1 drops recall for a selective included predicate');
cmp_ok($relaxed, '>', $low, 'raising max_probes under relaxed_order brings recall back');
cmp_ok($full, '>=', 0.99, 'probing every list restores recall');

my $with_filter = batch_seconds(20, 20, 'off', 'on');
my $without_filter = batch_seconds(20, 20, 'off', 'off');

diag("seconds include=on $with_filter include=off $without_filter");
cmp_ok($with_filter, '<', $without_filter,
	'same probe count is faster with the include filter');

done_testing();
