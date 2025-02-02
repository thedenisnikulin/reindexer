#include "join_selects_api.h"

TEST_F(JoinSelectsApi, JoinsDSLTest) {
	Query queryGenres(genres_namespace);
	Query queryAuthors(authors_namespace);
	Query queryBooks{Query(books_namespace, 0, 10).Where(price, CondGe, 500)};
	queryBooks.OrInnerJoin(genreId_fk, genreid, CondEq, std::move(queryGenres));
	queryBooks.LeftJoin(authorid_fk, authorid, CondEq, std::move(queryAuthors));

	string dsl = queryBooks.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(queryBooks == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, EqualPositionDSLTest) {
	Query query = Query(default_namespace);
	query.Where("f1", CondEq, 1).Where("f2", CondEq, 2).Or().Where("f3", CondEq, 2);
	query.AddEqualPosition({"f1", "f2"});
	query.AddEqualPosition({"f1", "f3"});
	query.OpenBracket().Where("f4", CondEq, 4).Where("f5", CondLt, 10);
	query.AddEqualPosition({"f4", "f5"});
	query.CloseBracket();

	string dsl = query.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, MergedQueriesDSLTest) {
	Query mainBooksQuery{Query(books_namespace, 0, 10).Where(price, CondGe, 500)};
	Query firstMergedQuery{Query(books_namespace, 10, 100).Where(pages, CondLe, 250)};
	Query secondMergedQuery{Query(books_namespace, 100, 50).Where(bookid, CondGe, 100)};

	mainBooksQuery.mergeQueries_.emplace_back(Merge, std::move(firstMergedQuery));
	mainBooksQuery.mergeQueries_.emplace_back(Merge, std::move(secondMergedQuery));

	string dsl = mainBooksQuery.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(mainBooksQuery == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, AggregateFunctonsDSLTest) {
	Query query{Query(books_namespace, 10, 100).Where(pages, CondGe, 150)};

	reindexer::AggregateEntry aggEntry;
	aggEntry.fields_ = {price};
	aggEntry.type_ = AggAvg;
	query.aggregations_.push_back(aggEntry);

	aggEntry.fields_ = {pages};
	aggEntry.type_ = AggSum;
	query.aggregations_.push_back(aggEntry);

	aggEntry.fields_ = {title, pages};
	aggEntry.type_ = AggFacet;
	aggEntry.sortingEntries_.push_back({title, true});
	aggEntry.limit_ = 100;
	aggEntry.offset_ = 10;
	query.aggregations_.push_back(aggEntry);

	string dsl = query.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, SelectFilterDSLTest) {
	Query query{Query(books_namespace, 10, 100).Where(pages, CondGe, 150)};
	query.selectFilter_.push_back(price);
	query.selectFilter_.push_back(pages);
	query.selectFilter_.push_back(title);

	string dsl = query.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, SelectFilterInJoinDSLTest) {
	Query queryBooks(books_namespace, 0, 10);
	queryBooks.selectFilter_.push_back(price);
	queryBooks.selectFilter_.push_back(title);
	{
		Query queryAuthors(authors_namespace);
		queryAuthors.selectFilter_.push_back(authorid);
		queryAuthors.selectFilter_.push_back(age);

		queryBooks.LeftJoin(authorid_fk, authorid, CondEq, std::move(queryAuthors));
	}
	string dsl = queryBooks.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_EQ(queryBooks, testLoadDslQuery);
}

TEST_F(JoinSelectsApi, ReqTotalDSLTest) {
	Query query{Query(books_namespace, 10, 100, ModeNoTotal).Where(pages, CondGe, 150)};

	string dsl1 = query.GetJSON();
	Query testLoadDslQuery1;
	Error err = testLoadDslQuery1.FromJSON(dsl1);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery1);

	query.CachedTotal();
	string dsl2 = query.GetJSON();
	Query testLoadDslQuery2;
	err = testLoadDslQuery2.FromJSON(dsl2);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery2);

	query.ReqTotal();
	string dsl3 = query.GetJSON();
	Query testLoadDslQuery3;
	err = testLoadDslQuery3.FromJSON(dsl3);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_EQ(query, testLoadDslQuery3);
}

TEST_F(JoinSelectsApi, SelectFunctionsDSLTest) {
	Query query{Query(books_namespace, 10, 100).Where(pages, CondGe, 150)};
	query.AddFunction("f1()");
	query.AddFunction("f2()");
	query.AddFunction("f3()");

	string dsl = query.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, CompositeValuesDSLTest) {
	string pagesBookidIndex = string(pages + string("+") + bookid);
	Query query{Query(books_namespace).WhereComposite(pagesBookidIndex.c_str(), CondGe, {{Variant(500), Variant(10)}})};
	string dsl = query.GetJSON();
	Query testLoadDslQuery;
	Error err = testLoadDslQuery.FromJSON(dsl);
	ASSERT_TRUE(err.ok()) << err.what();
	ASSERT_TRUE(query == testLoadDslQuery);
}

TEST_F(JoinSelectsApi, GeneralDSLTest) {
	Query queryGenres(genres_namespace);
	Query queryAuthors(authors_namespace);
	Query queryBooks{Query(books_namespace, 0, 10).Where(price, CondGe, 500)};
	Query innerJoinQuery = queryBooks.InnerJoin(authorid_fk, authorid, CondEq, std::move(queryAuthors));

	Query testDslQuery = innerJoinQuery.OrInnerJoin(genreId_fk, genreid, CondEq, std::move(queryGenres));
	testDslQuery.mergeQueries_.emplace_back(Merge, std::move(queryBooks));
	testDslQuery.mergeQueries_.emplace_back(Merge, std::move(innerJoinQuery));
	testDslQuery.selectFilter_.push_back(genreid);
	testDslQuery.selectFilter_.push_back(bookid);
	testDslQuery.selectFilter_.push_back(authorid_fk);
	testDslQuery.AddFunction("f1()");
	testDslQuery.AddFunction("f2()");

	reindexer::AggregateEntry aggEntry;
	aggEntry.fields_ = {bookid};
	aggEntry.type_ = AggDistinct;
	testDslQuery.aggregations_.push_back(aggEntry);

	Query testLoadDslQuery;
	const string dsl1 = testDslQuery.GetJSON();
	Error err = testLoadDslQuery.FromJSON(dsl1);
	EXPECT_TRUE(err.ok()) << err.what();
	EXPECT_TRUE(testDslQuery == testLoadDslQuery);
}
