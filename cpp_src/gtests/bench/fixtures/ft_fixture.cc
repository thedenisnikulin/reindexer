#include "ft_fixture.h"
#include <benchmark/benchmark.h>

#include <fstream>
#include <iterator>
#include <thread>

#include "core/cjson/jsonbuilder.h"
#include "core/ft/config/ftfastconfig.h"
#include "tools/stringstools.h"

using benchmark::State;
using benchmark::AllocsTracker;

using std::ifstream;
using std::istream_iterator;
using std::back_inserter;

using reindexer::Query;
using reindexer::QueryResults;
using reindexer::utf16_to_utf8;

uint8_t printFlags = AllocsTracker::kPrintAllocs | AllocsTracker::kPrintHold;

FullText::FullText(Reindexer* db, const string& name, size_t maxItems) : BaseFixture(db, name, maxItems, 1, false) {
	static reindexer::FtFastConfig ftCfg(1);
	static IndexOpts ftIndexOpts;
	ftCfg.optimization = reindexer::FtFastConfig::Optimization::Memory;
	ftIndexOpts.config = ftCfg.GetJson({});
	ftIndexOpts.Dense();
	nsdef_.AddIndex("id", "hash", "int", IndexOpts().PK())
		.AddIndex("description", "-", "string", IndexOpts())
		.AddIndex("year", "tree", "int", IndexOpts())
		.AddIndex("countries", "tree", "string", IndexOpts().Array())
		.AddIndex("searchfast", {"countries", "description"}, "text", "composite", ftIndexOpts)
		.AddIndex("searchfuzzy", {"countries", "description"}, "fuzzytext", "composite", IndexOpts());
}

template <reindexer::FtFastConfig::Optimization opt>
void FullText::UpdateIndex(State& state) {
	static reindexer::FtFastConfig ftCfg(1);
	ftCfg.optimization = opt;
	const auto it = std::find_if(nsdef_.indexes.begin(), nsdef_.indexes.end(), [](const auto& idx) { return idx.name_ == "searchfast"; });
	assert(it != nsdef_.indexes.end());
	it->opts_.config = ftCfg.GetJson({});
	AllocsTracker allocsTracker(state, printFlags);
	for (auto _ : state) {
		const auto err = db_->UpdateIndex(nsdef_.name, *it);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
	auto err = db_->Commit(nsdef_.name);
	if (!err.ok()) state.SkipWithError(err.what().c_str());
	// Worm up index
	Query q(nsdef_.name);
	q.Where("searchfast", CondEq, "lskfj");
	QueryResults qres;
	err = db_->Select(q, qres);
	if (!err.ok()) state.SkipWithError(err.what().c_str());
}

reindexer::Error FullText::Initialize() {
	auto err = BaseFixture::Initialize();
	if (!err.ok()) return err;

	ifstream file;
	file.open(RX_BENCH_DICT_PATH);

	if (!file) return Error(errNotValid, "%s", strerror(errno));
	words_.reserve(140000);
	std::copy(istream_iterator<string>(file), istream_iterator<string>(), back_inserter(words_));

	// clang-format off
	countries_ = {
		"Afghanistan", "Albania", "Algeria", "Andorra", "Angola", "Antigua & Deps",  "Argentina", "Armenia",
		"Australia", "Austria", "Azerbaijan", "Bahamas", "Bahrain", "Bangladesh", "Barbados", "Belarus",
		"Belgium", "Belize", "Benin", "Bhutan", "Bolivia", "Bosnia Herzegovina", "Botswana", "Brazil",
		"Brunei", "Bulgaria", "Burkina", "Burundi", "Cambodia", "Cameroon", "Canada", "Cape Verde"
		"Central African Rep", "Chad", "Chile", "China", "Colombia", "Comoros", "Congo", "Congo {Democratic Rep}",
		"Costa Rica", "Croatia", "Cuba", "Cyprus", "Czech Republic", "Denmark", "Djibouti", "Dominica",
		"Dominican Republic", "East Timor", "Ecuador", "Egypt", "El Salvador", "Equatorial Guinea", "Eritrea", "Estonia",
		"Ethiopia", "Fiji", "Finland", "France", "Gabon", "Gambia", "Georgia", "Germany",
		"Ghana", "Greece", "Grenada", "Guatemala", "Guinea", "Guinea-Bissau", "Guyana", "Haiti",
		"Honduras", "Hungary", "Iceland", "India", "Indonesia", "Iran", "Iraq", "Ireland {Republic}",
		"Israel", "Italy", "Ivory Coast", "Jamaica", "Japan", "Jordan", "Kazakhstan", "Kenya",
		"Kiribati", "Korea North", "Korea South", "Kosovo", "Kuwait", "Kyrgyzstan", "Laos", "Latvia",
		"Lebanon", "Lesotho", "Liberia", "Libya", "Liechtenstein", "Lithuania", "Luxembourg", "Macedonia",
		"Madagascar", "Malawi", "Malaysia", "Maldives", "Mali", "Malta", "Marshall Islands", "Mauritania",
		"Mauritius", "Mexico", "Micronesia", "Moldova", "Monaco", "Mongolia", "Montenegro", "Morocco",
		"Mozambique", "Myanmar, {Burma}", "Namibia", "Nauru", "Nepal", "Netherlands", "New Zealand",
		"Nicaragua", "Niger", "Nigeria", "Norway", "Oman", "Pakistan", "Palau", "Panama",
		"Papua New Guinea", "Paraguay", "Peru", "Philippines", "Poland", "Portugal", "Qatar", "Romania",
		"Russian Federation", "Rwanda", "St Kitts & Nevis", "St Lucia", "Saint Vincent & the Grenadines", "Samoa", "San Marino", "Sao Tome & Principe",
		"Saudi Arabia", "Senegal", "Serbia", "Seychelles", "Sierra Leone", "Singapore", "Slovakia", "Slovenia",
		"Solomon Islands", "Somalia", "South Africa", "South Sudan", "Spain", "Sri Lanka", "Sudan", "Suriname",
		"Swaziland", "Sweden", "Switzerland", "Syria", "Taiwan", "Tajikistan", "Tanzania", "Thailand",
		"Togo", "Tonga", "Trinidad & Tobago", "Tunisia", "Turkey", "Turkmenistan", "Tuvalu", "Uganda",
		"Ukraine", "United Arab Emirates", "United Kingdom", "United States", "Uruguay", "Uzbekistan", "Vanuatu", "Vatican City",
		"Venezuela", "Vietnam", "Yemen", "Zambia", "Zimbabwe"
	};
	// clang-format on
	return 0;
}

void FullText::RegisterAllCases() {
	constexpr static auto Mem = reindexer::FtFastConfig::Optimization::Memory;
	constexpr static auto CPU = reindexer::FtFastConfig::Optimization::CPU;
	Register("Insert", &FullText::Insert, this)->Iterations(id_seq_->Count())->Unit(benchmark::kMicrosecond);

	// Register("BuildCommonIndexes", &FullText::BuildCommonIndexes, this)->Iterations(1)->Unit(benchmark::kMicrosecond);
	Register("BuildFastTextIndex", &FullText::BuildFastTextIndex, this)->Iterations(1)->Unit(benchmark::kMicrosecond);
	// Register("BuildFuzzyTextIndex", &FullText::BuildFuzzyTextIndex, this)->Iterations(1)->Unit(benchmark::kMicrosecond);

	Register("Fast1WordMatch.OptByMem", &FullText::Fast1WordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2WordsMatch.OptByMem", &FullText::Fast2WordsMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1PrefixMatch.OptByMem", &FullText::Fast1PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2PrefixMatch.OptByMem", &FullText::Fast2PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1SuffixMatch.OptByMem", &FullText::Fast1SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2SuffixMatch.OptByMem", &FullText::Fast2SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1TypoWordMatch.OptByMem", &FullText::Fast1TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2TypoWordMatch.OptByMem", &FullText::Fast2TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("SetOptimizationByCPU", &FullText::UpdateIndex<CPU>, this)->Iterations(1)->Unit(benchmark::kMicrosecond);
	Register("Fast1WordMatch.OptByCPU", &FullText::Fast1WordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2WordsMatch.OptByCPU", &FullText::Fast2WordsMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1PrefixMatch.OptByCPU", &FullText::Fast1PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2PrefixMatch.OptByCPU", &FullText::Fast2PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1SuffixMatch.OptByCPU", &FullText::Fast1SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2SuffixMatch.OptByCPU", &FullText::Fast2SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1TypoWordMatch.OptByCPU", &FullText::Fast1TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2TypoWordMatch.OptByCPU", &FullText::Fast2TypoWordMatch, this)->Unit(benchmark::kMicrosecond);

	// Register("Fuzzy1WordMatch", &FullText::Fuzzy1WordMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy2WordsMatch", &FullText::Fuzzy2WordsMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy1PrefixMatch", &FullText::Fuzzy1PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy2PrefixMatch", &FullText::Fuzzy2PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy1SuffixMatch", &FullText::Fuzzy1SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy2SuffixMatch", &FullText::Fuzzy2SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy1TypoWordMatch", &FullText::Fuzzy1TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	// Register("Fuzzy2TypoWordMatch", &FullText::Fuzzy2TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("SetOptimizationByMemory", &FullText::UpdateIndex<Mem>, this)->Iterations(1)->Unit(benchmark::kMicrosecond);
	Register("BuildInsertSteps", &FullText::BuildInsertSteps, this)->Iterations(id_seq_->Count())->Unit(benchmark::kMicrosecond);
	Register("Fast1WordMatchSteps.OptByMem", &FullText::Fast1WordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2WordsMatchSteps.OptByMem", &FullText::Fast2WordsMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1PrefixMatchSteps.OptByMem", &FullText::Fast1PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2PrefixMatchSteps.OptByMem", &FullText::Fast2PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1SuffixMatchSteps.OptByMem", &FullText::Fast1SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2SuffixMatchSteps.OptByMem", &FullText::Fast2SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1TypoWordMatchSteps.OptByMem", &FullText::Fast1TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2TypoWordMatchSteps.OptByMem", &FullText::Fast2TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("SetOptimizationByCPU", &FullText::UpdateIndex<CPU>, this)->Iterations(1)->Unit(benchmark::kMicrosecond);
	Register("Fast1WordMatch.OptByCPU", &FullText::Fast1WordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2WordsMatch.OptByCPU", &FullText::Fast2WordsMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1PrefixMatch.OptByCPU", &FullText::Fast1PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2PrefixMatch.OptByCPU", &FullText::Fast2PrefixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1SuffixMatch.OptByCPU", &FullText::Fast1SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2SuffixMatch.OptByCPU", &FullText::Fast2SuffixMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast1TypoWordMatch.OptByCPU", &FullText::Fast1TypoWordMatch, this)->Unit(benchmark::kMicrosecond);
	Register("Fast2TypoWordMatch.OptByCPU", &FullText::Fast2TypoWordMatch, this)->Unit(benchmark::kMicrosecond);

	Register("InitForAlternatingUpdatesAndSelects.OptByMem", &FullText::InitForAlternatingUpdatesAndSelects<Mem>, this)
		->Iterations(1)
		->Unit(benchmark::kMicrosecond);
	Register("AlternatingUpdatesAndSelects.OptByMem", &FullText::AlternatingUpdatesAndSelects, this)->Unit(benchmark::kMicrosecond);
	Register("AlternatingUpdatesAndSelectsByComposite.OptByMem", &FullText::AlternatingUpdatesAndSelectsByComposite, this)
		->Unit(benchmark::kMicrosecond);
	Register("AlternatingUpdatesAndSelectsByCompositeByNotIndexFields.OptByMem",
			 &FullText::AlternatingUpdatesAndSelectsByCompositeByNotIndexFields, this)
		->Unit(benchmark::kMicrosecond);
	Register("InitForAlternatingUpdatesAndSelects.OptByCPU", &FullText::InitForAlternatingUpdatesAndSelects<CPU>, this)
		->Iterations(1)
		->Unit(benchmark::kMicrosecond);
	Register("AlternatingUpdatesAndSelects.OptByCPU", &FullText::AlternatingUpdatesAndSelects, this)->Unit(benchmark::kMicrosecond);
	Register("AlternatingUpdatesAndSelectsByComposite.OptByCPU", &FullText::AlternatingUpdatesAndSelectsByComposite, this)
		->Unit(benchmark::kMicrosecond);
	Register("AlternatingUpdatesAndSelectsByCompositeByNotIndexFields.OptByCPU",
			 &FullText::AlternatingUpdatesAndSelectsByCompositeByNotIndexFields, this)
		->Unit(benchmark::kMicrosecond);
}

reindexer::Item FullText::MakeSpecialItem() {
	auto item = db_->NewItem(nsdef_.name);
	item.Unsafe(false);

	item["id"] = id_seq_->Next();
	item["description"] = RandString();

	return item;
}

reindexer::Item FullText::MakeItem() {
	auto item = db_->NewItem(nsdef_.name);
	item.Unsafe(false);

	auto phrase = CreatePhrase();
	auto countries = GetRandomCountries();
	raw_data_sz_ += phrase.size();
	for (auto& c : countries) raw_data_sz_ += c.size();

	item["id"] = id_seq_->Next();
	item["description"] = phrase;
	item["year"] = random<int>(2000, 2049);
	item["countries"] = toArray<string>(countries);

	return item;
}

void FullText::BuildInsertSteps(State& state) {
	AllocsTracker allocsTracker(state, printFlags);

	db_->DropNamespace(nsdef_.name);
	auto err = BaseFixture::Initialize();
	nsdef_.AddIndex("id", "hash", "int", IndexOpts().PK()).AddIndex("description", "-", "string", IndexOpts());
	size_t i = 0;
	size_t mem = 0;

	for (auto _ : state) {
		auto item = MakeSpecialItem();
		if (!item.Status().ok()) state.SkipWithError(item.Status().what().c_str());

		auto err = db_->Insert(nsdef_.name, item);
		if (!err.ok()) state.SkipWithError(err.what().c_str());

		if (i % 12000 == 0) {
			Query q(nsdef_.name);
			q.Where("searchfast", CondEq, words_.at(random<size_t>(0, words_.size() - 1))).Limit(20);

			QueryResults qres;
			size_t memory = get_alloc_size();
			auto err = db_->Select(q, qres);

			memory = get_alloc_size() - memory;
			if (!err.ok()) state.SkipWithError(err.what().c_str());

			mem += memory;
		}
		++i;
	}
	double ratio = mem / double(raw_data_sz_);
	state.SetLabel("Commit ratio: " + std::to_string(ratio));
}

void FullText::Insert(State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	for (auto _ : state) {
		auto item = MakeItem();
		if (!item.Status().ok()) state.SkipWithError(item.Status().what().c_str());

		auto err = db_->Insert(nsdef_.name, item);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}

	auto err = db_->Commit(nsdef_.name);
	if (!err.ok()) state.SkipWithError(err.what().c_str());
}

void FullText::BuildCommonIndexes(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	for (auto _ : state) {
		Query q(nsdef_.name);
		q.Where("year", CondRange, {2010, 2016}).Limit(20).Sort("year", false);
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
}

void FullText::BuildFastTextIndex(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t mem = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);
		q.Where("searchfast", CondEq, words_.at(random<size_t>(0, words_.size() - 1))).Limit(20);

		QueryResults qres;

		mem = get_alloc_size();
		auto err = db_->Select(q, qres);
		mem = get_alloc_size() - mem;

		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
	double ratio = mem / double(raw_data_sz_);
	state.SetLabel("Commit ratio: " + std::to_string(ratio));
}

void FullText::BuildFuzzyTextIndex(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t mem = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);
		q.Where("searchfuzzy", CondEq, words_.at(random<size_t>(0, words_.size() - 1))).Limit(20);

		QueryResults qres;

		mem = get_alloc_size();
		auto err = db_->Select(q, qres);
		mem = get_alloc_size() - mem;

		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
	double ratio = mem / double(raw_data_sz_);
	state.SetLabel("Commit ratio: " + std::to_string(ratio));
}

void FullText::Fast1WordMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		q.Where("searchfast", CondEq, words_.at(random<size_t>(0, words_.size() - 1)));

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast2WordsMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);
		string words = words_.at(random<size_t>(0, words_.size() - 1)) + " " + words_.at(random<size_t>(0, words_.size() - 1));

		q.Where("searchfast", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy1WordMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		q.Where("searchfuzzy", CondEq, words_.at(random<size_t>(0, words_.size() - 1)));

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy2WordsMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);
		string words = words_.at(random<size_t>(0, words_.size() - 1)) + " " + words_.at(random<size_t>(0, words_.size() - 1));

		q.Where("searchfuzzy", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast1PrefixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		auto word = MakePrefixWord();
		q.Where("searchfast", CondEq, word);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast2PrefixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		auto words = MakePrefixWord() + " " + MakePrefixWord();
		q.Where("searchfast", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy1PrefixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		auto word = MakePrefixWord();
		q.Where("searchfuzzy", CondEq, word);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy2PrefixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string words = MakePrefixWord() + " " + MakePrefixWord();
		q.Where("searchfuzzy", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast1SuffixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string word = MakeSuffixWord();
		q.Where("searchfast", CondEq, word);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast2SuffixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string words = MakeSuffixWord() + " " + MakeSuffixWord();
		q.Where("searchfast", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy1SuffixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string word = MakeSuffixWord();
		q.Where("searchfuzzy", CondEq, word);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy2SuffixMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string words = MakeSuffixWord() + " " + MakeSuffixWord();
		q.Where("searchfuzzy", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast1TypoWordMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string word = MakeTypoWord();
		q.Where("searchfast", CondEq, word);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fast2TypoWordMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string words = MakeTypoWord() + " " + MakeTypoWord();
		q.Where("searchfast", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy1TypoWordMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string word = MakeTypoWord();
		q.Where("searchfuzzy", CondEq, word);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

void FullText::Fuzzy2TypoWordMatch(benchmark::State& state) {
	AllocsTracker allocsTracker(state, printFlags);
	size_t cnt = 0;
	for (auto _ : state) {
		Query q(nsdef_.name);

		string words = MakeTypoWord() + " " + MakeTypoWord();
		q.Where("searchfuzzy", CondEq, words);

		QueryResults qres;
		auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		cnt += qres.Count();
	}
	state.SetLabel(FormatString("RPR: %.1f", cnt / double(state.iterations())));
}

string FullText::CreatePhrase() {
	size_t wordCnt = 100;
	reindexer::WrSerializer r;
	r.Reserve(wordCnt * 30);

	for (size_t i = 0; i < wordCnt; i++) {
		r << words_.at(random<size_t>(0, words_.size() - 1));
		if (i < wordCnt - 1) r << " ";
	}

	return string(r.Slice());
}

string FullText::MakePrefixWord() {
	auto word = GetRandomUTF16WordByLength(4);

	auto pos = random<wstring::size_type>(2, word.length() - 2);
	word.erase(pos, word.length() - pos);
	word += L"*";

	return reindexer::utf16_to_utf8(word);
}

string FullText::MakeSuffixWord() {
	auto word = GetRandomUTF16WordByLength(4);
	auto cnt = random<wstring::size_type>(0, word.length() / 2);
	word.erase(0, cnt);
	word = L"*" + word;
	return utf16_to_utf8(word);
}

string FullText::MakeTypoWord() {
	static const wstring wchars =
		L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZабвгдежзийклмнопрстуфхцчшщъыьэюяАБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ";
	auto word = GetRandomUTF16WordByLength(2);
	word[random<wstring::size_type>(0, word.length() - 1)] = wchars.at(random<wstring::size_type>(0, wchars.size() - 1));
	word += L"~";
	return utf16_to_utf8(word);
}

wstring FullText::GetRandomUTF16WordByLength(size_t minLen) {
	wstring word;
	for (; word.length() < minLen;) {
		auto& w = words_.at(random<size_t>(0, words_.size() - 1));
		word = reindexer::utf8_to_utf16(w);
	}
	return word;
}

vector<std::string> FullText::GetRandomCountries(size_t cnt) {
	vector<string> result;
	result.reserve(cnt);
	for (auto i = cnt; i > 0; i--) {
		result.emplace_back(countries_.at(random<size_t>(0, countries_.size() - 1)));
	}
	return result;
}

template <reindexer::FtFastConfig::Optimization opt>
void FullText::InitForAlternatingUpdatesAndSelects(State& state) {
	constexpr int kNsSize = 100'000;
	static reindexer::FtFastConfig ftCfg(1);
	static IndexOpts ftIndexOpts;
	ftCfg.optimization = opt;
	ftIndexOpts.config = ftCfg.GetJson({});
	AllocsTracker allocsTracker(state, printFlags);
	db_->DropNamespace(alternatingNs_);
	for (auto _ : state) {
		NamespaceDef nsDef{alternatingNs_};
		nsDef.AddIndex("id", "hash", "int", IndexOpts().PK())
			.AddIndex("search1", "text", "string", ftIndexOpts)
			.AddIndex("search2", "text", "string", ftIndexOpts)
			.AddIndex("rand", "hash", "int", IndexOpts())
			.AddIndex("search_comp", {"search1", "search2"}, "text", "composite", ftIndexOpts)
			.AddIndex("search_comp_not_index_fields", {"field1", "field2"}, "text", "composite", ftIndexOpts);
		auto err = db_->AddNamespace(nsDef);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
		values_.clear();
		values_.reserve(kNsSize);
		reindexer::WrSerializer ser;
		for (int i = 0; i < kNsSize; ++i) {
			values_.emplace_back(RandString(), RandString(), RandString(), RandString());
			ser.Reset();
			reindexer::JsonBuilder bld(ser);
			bld.Put("id", i);
			bld.Put("search1", values_.back().search1);
			bld.Put("search2", values_.back().search2);
			bld.Put("field1", values_.back().field1);
			bld.Put("field2", values_.back().field2);
			bld.Put("rand", rand());
			bld.End();
			auto item = db_->NewItem(alternatingNs_);
			if (!item.Status().ok()) state.SkipWithError(item.Status().what().c_str());
			err = item.FromJSON(ser.Slice());
			if (!err.ok()) state.SkipWithError(err.what().c_str());
			err = db_->Insert(alternatingNs_, item);
			if (!err.ok()) state.SkipWithError(err.what().c_str());
		}
	}

	auto err = db_->Commit(alternatingNs_);
	if (!err.ok()) state.SkipWithError(err.what().c_str());

	// Init index build
	Query q = Query(alternatingNs_).Where("search1", CondEq, values_[random<size_t>(0, values_.size() - 1)].search1);
	QueryResults qres;
	err = db_->Select(q, qres);
	if (!err.ok()) state.SkipWithError(err.what().c_str());

	size_t index = random<size_t>(0, values_.size() - 1);
	q = Query(alternatingNs_).Where("search_comp", CondEq, values_[index].search1 + ' ' + values_[index].search2);
	qres.Clear();
	err = db_->Select(q, qres);
	if (!err.ok()) state.SkipWithError(err.what().c_str());

	index = random<size_t>(0, values_.size() - 1);
	q = Query(alternatingNs_).Where("search_comp_not_index_fields", CondEq, values_[index].field1 + ' ' + values_[index].field2);
	qres.Clear();
	err = db_->Select(q, qres);
	if (!err.ok()) state.SkipWithError(err.what().c_str());
}

void FullText::updateAlternatingNs(reindexer::WrSerializer& ser, benchmark::State& state) {
	using namespace std::string_literals;
	const int i = random<int>(0, values_.size() - 1);
	ser.Reset();
	reindexer::JsonBuilder bld(ser);
	bld.Put("id", i);
	bld.Put("search1", values_[i].search1);
	bld.Put("search2", values_[i].search2);
	bld.Put("field1", values_[i].field1);
	bld.Put("field2", values_[i].field2);
	bld.Put("rand", rand());
	bld.End();
	auto item = db_->NewItem(alternatingNs_);
	item.Unsafe(false);
	if (!item.Status().ok()) state.SkipWithError(item.Status().what().c_str());
	auto err = item.FromJSON(ser.Slice());
	if (!err.ok()) state.SkipWithError(err.what().c_str());
	err = db_->Update(alternatingNs_, item);
	if (!err.ok()) state.SkipWithError(err.what().c_str());

	const std::string sql = "UPDATE "s + alternatingNs_ + " SET rand = " + std::to_string(rand()) +
							" WHERE id = " + std::to_string(random<int>(0, values_.size() - 1));
	QueryResults qres;
	err = db_->Select(sql, qres);
	if (!err.ok()) state.SkipWithError(err.what().c_str());
}

void FullText::AlternatingUpdatesAndSelects(benchmark::State& state) {
	reindexer::WrSerializer ser;
	AllocsTracker allocsTracker(state, printFlags);
	for (auto _ : state) {
		state.PauseTiming();
		updateAlternatingNs(ser, state);
		Query q = Query(alternatingNs_).Where("search1", CondEq, values_[random<size_t>(0, values_.size() - 1)].search1);
		QueryResults qres;
		state.ResumeTiming();
		const auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
}

void FullText::AlternatingUpdatesAndSelectsByComposite(benchmark::State& state) {
	reindexer::WrSerializer ser;
	AllocsTracker allocsTracker(state, printFlags);
	for (auto _ : state) {
		state.PauseTiming();
		updateAlternatingNs(ser, state);
		const size_t index = random<size_t>(0, values_.size() - 1);
		Query q = Query(alternatingNs_).Where("search_comp", CondEq, values_[index].search1 + ' ' + values_[index].search2);
		QueryResults qres;
		state.ResumeTiming();
		const auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
}

void FullText::AlternatingUpdatesAndSelectsByCompositeByNotIndexFields(benchmark::State& state) {
	reindexer::WrSerializer ser;
	AllocsTracker allocsTracker(state, printFlags);
	for (auto _ : state) {
		state.PauseTiming();
		updateAlternatingNs(ser, state);
		const size_t index = random<size_t>(0, values_.size() - 1);
		Query q = Query(alternatingNs_).Where("search_comp_not_index_fields", CondEq, values_[index].field1 + ' ' + values_[index].field2);
		QueryResults qres;
		state.ResumeTiming();
		const auto err = db_->Select(q, qres);
		if (!err.ok()) state.SkipWithError(err.what().c_str());
	}
}
