#include "composite_indexes_api.h"

TEST_F(CompositeIndexesApi, CompositeIndexesAddTest) {
	addCompositeIndex({kFieldNameBookid, kFieldNameBookid2}, CompositeIndexHash, IndexOpts().PK());
	fillNamespace(0, 100);
	addCompositeIndex({kFieldNameTitle, kFieldNamePages}, CompositeIndexHash, IndexOpts());
	fillNamespace(101, 200);
	addCompositeIndex({kFieldNameTitle, kFieldNamePrice}, CompositeIndexBTree, IndexOpts());
	fillNamespace(201, 300);
}

TEST_F(CompositeIndexesApi, AddIndexWithExistingCompositeIndex) {
	static constexpr const char* namespaceName = "test_ns_add_index";
	static const std::string kFieldNameComposite = std::string{kFieldNameName} + compositePlus + kFieldNameTitle;
	static const std::vector<std::string> suffixes = {"Vol1", "Vol2", "Vol3"};
	static constexpr int size = 10;
	std::vector<std::string> names;
	names.reserve(size);
	for (int i = 0; i < size; ++i) {
		names.push_back(kFieldNameName + suffixes[i % suffixes.size()]);
	}

	Error err = rt.reindexer->OpenNamespace(namespaceName);
	ASSERT_TRUE(err.ok()) << err.what();
	DefineNamespaceDataset(namespaceName, {IndexDeclaration{kFieldNameBookid, "hash", "int", IndexOpts().PK(), 0},
										   IndexDeclaration{kFieldNameBookid2, "hash", "int", IndexOpts(), 0},
										   IndexDeclaration{kFieldNamePages, "hash", "int", IndexOpts(), 0},
										   IndexDeclaration{kFieldNamePrice, "hash", "int", IndexOpts(), 0},
										   IndexDeclaration{kFieldNameComposite.c_str(), "text", "composite", IndexOpts(), 0}});
	for (int i = 0; i < size; ++i) {
		Item item = NewItem(namespaceName);
		item[this->kFieldNameBookid] = i;
		item[this->kFieldNameBookid2] = 777;
		item[this->kFieldNamePages] = 1010;
		item[this->kFieldNamePrice] = 1200;
		item[this->kFieldNameName] = names[i];
		item[this->kFieldNameTitle] = kFieldNameTitle;
		Upsert(namespaceName, item);
		Commit(namespaceName);
	}
	err = rt.reindexer->AddIndex(namespaceName, {kFieldNameName, {kFieldNameName}, "text", "string", IndexOpts()});
	ASSERT_TRUE(err.ok()) << err.what();
}

void selectAll(reindexer::Reindexer* reindexer, const string& ns) {
	QueryResults qr;
	Error err = reindexer->Select(Query(ns, 0, 1000, ModeAccurateTotal), qr);
	EXPECT_TRUE(err.ok()) << err.what();

	for (auto it : qr) {
		reindexer::WrSerializer wrser;
		it.GetJSON(wrser, false);
	}
}

TEST_F(CompositeIndexesApi, DropTest2) {
	const string test_ns = "weird_namespace";
	auto err = rt.reindexer->OpenNamespace(test_ns, StorageOpts().Enabled(false));
	EXPECT_TRUE(err.ok()) << err.what();

	err = rt.reindexer->AddIndex(test_ns, {"id", "hash", "int", IndexOpts().PK().Dense()});
	EXPECT_TRUE(err.ok()) << err.what();

	for (int i = 0; i < 1000; ++i) {
		Item item = NewItem(test_ns);
		EXPECT_TRUE(!!item);
		EXPECT_TRUE(item.Status().ok()) << item.Status().what();

		item["id"] = i + 1;

		err = rt.reindexer->Upsert(test_ns, item);
		EXPECT_TRUE(err.ok()) << err.what();
	}

	err = rt.reindexer->Commit(test_ns);
	EXPECT_TRUE(err.ok()) << err.what();

	selectAll(rt.reindexer.get(), test_ns);

	reindexer::IndexDef idef("id");
	err = rt.reindexer->DropIndex(test_ns, idef);
	EXPECT_TRUE(err.ok()) << err.what();

	err = rt.reindexer->Commit(test_ns);
	EXPECT_TRUE(err.ok()) << err.what();

	selectAll(rt.reindexer.get(), test_ns);
}

TEST_F(CompositeIndexesApi, CompositeIndexesDropTest) {
	addCompositeIndex({kFieldNameBookid, kFieldNameBookid2}, CompositeIndexHash, IndexOpts().PK());
	fillNamespace(0, 100);
	addCompositeIndex({kFieldNameTitle, kFieldNamePages}, CompositeIndexHash, IndexOpts());
	fillNamespace(101, 200);
	addCompositeIndex({kFieldNameTitle, kFieldNamePrice}, CompositeIndexBTree, IndexOpts());
	fillNamespace(201, 300);

	dropIndex(getCompositeIndexName({kFieldNameTitle, kFieldNamePrice}));
	fillNamespace(401, 500);
	dropIndex(getCompositeIndexName({kFieldNameTitle, kFieldNamePages}));
	fillNamespace(601, 700);
}

TEST_F(CompositeIndexesApi, CompositeIndexesSelectTest) {
	int priceValue = 77777, pagesValue = 88888;
	const char* titleValue = "test book1 title";
	const char* nameValue = "test book1 name";

	addCompositeIndex({kFieldNameBookid, kFieldNameBookid2}, CompositeIndexHash, IndexOpts().PK());
	fillNamespace(0, 100);

	string compositeIndexName(getCompositeIndexName({kFieldNamePrice, kFieldNamePages}));
	addCompositeIndex({kFieldNamePrice, kFieldNamePages}, CompositeIndexHash, IndexOpts());

	addOneRow(300, 3000, titleValue, pagesValue, priceValue, nameValue);
	fillNamespace(101, 200);

	auto qr = execAndCompareQuery(
		Query(default_namespace).WhereComposite(compositeIndexName.c_str(), CondEq, {{Variant(priceValue), Variant(pagesValue)}}));
	EXPECT_TRUE(qr.Count() == 1);

	Item pricePageRow = qr.begin().GetItem(false);
	Variant selectedPrice = pricePageRow[kFieldNamePrice];
	Variant selectedPages = pricePageRow[kFieldNamePages];
	EXPECT_EQ(static_cast<int>(selectedPrice), priceValue);
	EXPECT_EQ(static_cast<int>(selectedPages), pagesValue);

	Item titleNameRow = qr.begin().GetItem(false);
	Variant selectedTitle = titleNameRow[kFieldNameTitle];
	Variant selectedName = titleNameRow[kFieldNameName];
	EXPECT_TRUE(static_cast<reindexer::key_string>(selectedTitle)->compare(string(titleValue)) == 0);
	EXPECT_TRUE(static_cast<reindexer::key_string>(selectedName)->compare(string(nameValue)) == 0);

	execAndCompareQuery(Query(default_namespace).WhereComposite(compositeIndexName, CondLt, {{Variant(priceValue), Variant(pagesValue)}}));
	execAndCompareQuery(Query(default_namespace).WhereComposite(compositeIndexName, CondLe, {{Variant(priceValue), Variant(pagesValue)}}));
	execAndCompareQuery(Query(default_namespace).WhereComposite(compositeIndexName, CondGt, {{Variant(priceValue), Variant(pagesValue)}}));
	execAndCompareQuery(Query(default_namespace).WhereComposite(compositeIndexName, CondGe, {{Variant(priceValue), Variant(pagesValue)}}));

	fillNamespace(301, 400);

	execAndCompareQuery(
		Query(default_namespace)
			.WhereComposite(compositeIndexName, CondRange, {{Variant(1), Variant(1)}, {Variant(priceValue), Variant(pagesValue)}}));

	vector<VariantArray> intKeys;
	for (int i = 0; i < 10; ++i) {
		intKeys.emplace_back(VariantArray{Variant(i), Variant(i * 5)});
	}
	execAndCompareQuery(Query(default_namespace).WhereComposite(compositeIndexName.c_str(), CondSet, intKeys));

	dropIndex(compositeIndexName);
	fillNamespace(401, 500);

	string compositeIndexName2(getCompositeIndexName({kFieldNameTitle, kFieldNameName}));
	addCompositeIndex({kFieldNameTitle, kFieldNameName}, CompositeIndexBTree, IndexOpts());

	fillNamespace(700, 200);

	execAndCompareQuery(
		Query(default_namespace)
			.WhereComposite(compositeIndexName2.c_str(), CondEq, {{Variant(string(titleValue)), Variant(string(nameValue))}}));
	execAndCompareQuery(
		Query(default_namespace)
			.WhereComposite(compositeIndexName2.c_str(), CondGe, {{Variant(string(titleValue)), Variant(string(nameValue))}}));
	execAndCompareQuery(
		Query(default_namespace)
			.WhereComposite(compositeIndexName2.c_str(), CondLt, {{Variant(string(titleValue)), Variant(string(nameValue))}}));
	execAndCompareQuery(
		Query(default_namespace)
			.WhereComposite(compositeIndexName2.c_str(), CondLe, {{Variant(string(titleValue)), Variant(string(nameValue))}}));

	fillNamespace(1200, 1000);

	vector<VariantArray> stringKeys;
	for (size_t i = 0; i < 1010; ++i) {
		stringKeys.emplace_back(VariantArray{Variant(RandString()), Variant(RandString())});
	}
	execAndCompareQuery(Query(default_namespace).WhereComposite(compositeIndexName2.c_str(), CondSet, stringKeys));
	execAndCompareQuery(
		Query(default_namespace)
			.Where(kFieldNameName, CondEq, nameValue)
			.WhereComposite(compositeIndexName2.c_str(), CondEq, {{Variant(string(titleValue)), Variant(string(nameValue))}}));

	dropIndex(compositeIndexName2);
	fillNamespace(201, 300);

	execAndCompareQuery(Query(default_namespace));
}
