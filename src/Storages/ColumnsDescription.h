#pragma once

#include <Core/Block.h>
#include <Core/Names.h>
#include <Core/NamesAndAliases.h>
#include <Core/NamesAndTypes.h>
#include <Interpreters/Context_fwd.h>
#include <Storages/ColumnDefault.h>
#include <Storages/StatisticsDescription.h>
#include <Common/Exception.h>
#include <Common/NamePrompter.h>
#include <Common/SettingsChanges.h>

#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <optional>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

enum class VirtualsKind : UInt8
{
    None = 0,
    Ephemeral = 1,
    Persistent = 2,
    All = Ephemeral | Persistent,
};

struct GetColumnsOptions
{
    enum Kind : UInt8
    {
        None = 0,
        Ordinary = 1,
        Materialized = 2,
        Aliases = 4,
        Ephemeral = 8,
        OrdinaryAndAliases = Ordinary | Aliases,
        AllPhysical = Ordinary | Materialized,
        AllPhysicalAndAliases = AllPhysical | Aliases,
        All = AllPhysical | Aliases | Ephemeral,
    };

    GetColumnsOptions(Kind kind_) : kind(kind_) {} /// NOLINT(google-explicit-constructor)

    GetColumnsOptions & withSubcolumns(bool value = true)
    {
        with_subcolumns = value;
        return *this;
    }

    GetColumnsOptions & withVirtuals(VirtualsKind value = VirtualsKind::All)
    {
        virtuals_kind = value;
        return *this;
    }

    GetColumnsOptions & withExtendedObjects(bool value = true)
    {
        with_extended_objects = value;
        return *this;
    }

    Kind kind;
    VirtualsKind virtuals_kind = VirtualsKind::None;

    bool with_subcolumns = false;
    bool with_extended_objects = false;
};

/// Description of a single table column (in CREATE TABLE for example).
struct ColumnDescription
{
    String name;
    DataTypePtr type;
    ColumnDefault default_desc;
    String comment;
    ASTPtr codec;
    SettingsChanges settings;
    ASTPtr ttl;
    ColumnStatisticsDescription statistics;

    ColumnDescription() = default;
    ColumnDescription(const ColumnDescription & other) { *this = other; }
    ColumnDescription & operator=(const ColumnDescription & other);
    ColumnDescription(ColumnDescription && other) noexcept { *this = std::move(other); }
    ColumnDescription & operator=(ColumnDescription && other) noexcept;

    ColumnDescription(String name_, DataTypePtr type_);
    ColumnDescription(String name_, DataTypePtr type_, String comment_);
    ColumnDescription(String name_, DataTypePtr type_, ASTPtr codec_, String comment_);

    bool operator==(const ColumnDescription & other) const;
    bool operator!=(const ColumnDescription & other) const { return !(*this == other); }

    void writeText(WriteBuffer & buf, IAST::FormatState & state, bool include_comment) const;
    void readText(ReadBuffer & buf);
};


/// Description of multiple table columns (in CREATE TABLE for example).
class ColumnsDescription : public IHints<>
{
public:
    ColumnsDescription() = default;

    static ColumnsDescription fromNamesAndTypes(NamesAndTypes ordinary);

    explicit ColumnsDescription(NamesAndTypesList ordinary);

    ColumnsDescription(std::initializer_list<ColumnDescription> ordinary);

    explicit ColumnsDescription(NamesAndTypesList ordinary, NamesAndAliases aliases);

    void setAliases(NamesAndAliases aliases);

    /// `after_column` can be a Nested column name;
    void add(ColumnDescription column, const String & after_column = String(), bool first = false, bool add_subcolumns = true);
    /// `column_name` can be a Nested column name;
    void remove(const String & column_name);

    /// Rename column. column_from and column_to cannot be nested columns.
    /// TODO add ability to rename nested columns
    void rename(const String & column_from, const String & column_to);

    /// NOTE Must correspond with Nested::flatten function.
    void flattenNested(); /// TODO: remove, insert already flattened Nested columns.

    bool operator==(const ColumnsDescription & other) const { return toString(false) == other.toString(false); }
    bool operator!=(const ColumnsDescription & other) const { return !(*this == other); }

    auto begin() const { return columns.begin(); }
    auto end() const { return columns.end(); }

    NamesAndTypesList get(const GetColumnsOptions & options) const;
    NamesAndTypesList getByNames(const GetColumnsOptions & options, const Names & names) const;

    NamesAndTypesList getOrdinary() const;
    NamesAndTypesList getMaterialized() const;
    NamesAndTypesList getInsertable() const; /// ordinary + ephemeral
    NamesAndTypesList getAliases() const;
    NamesAndTypesList getEphemeral() const;
    NamesAndTypesList getAllPhysical() const; /// ordinary + materialized.
    NamesAndTypesList getAll() const; /// ordinary + materialized + aliases + ephemeral
    /// Returns .size0/.null/...
    NamesAndTypesList getSubcolumns(const String & name_in_storage) const;
    /// Returns column_name.*
    NamesAndTypesList getNested(const String & column_name) const;

    using ColumnTTLs = std::unordered_map<String, ASTPtr>;
    ColumnTTLs getColumnTTLs() const;
    void resetColumnTTLs();

    bool has(const String & column_name) const;
    bool hasNested(const String & column_name) const;
    bool hasSubcolumn(const String & column_name) const;
    const ColumnDescription & get(const String & column_name) const;
    const ColumnDescription * tryGet(const String & column_name) const;

    template <typename F>
    void modify(const String & column_name, F && f)
    {
        modify(column_name, String(), false, std::forward<F>(f));
    }

    template <typename F>
    void modify(const String & column_name, const String & after_column, bool first, F && f)
    {
        auto it = columns.get<1>().find(column_name);
        if (it == columns.get<1>().end())
        {
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot find column {} in ColumnsDescription{}",
                            column_name, getHintsMessage(column_name));
        }

        removeSubcolumns(it->name);
        if (!columns.get<1>().modify(it, std::forward<F>(f)))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot modify ColumnDescription for column {}: column name cannot be changed", column_name);

        addSubcolumns(it->name, it->type);
        modifyColumnOrder(column_name, after_column, first);
    }

    Names getNamesOfPhysical() const;

    bool hasPhysical(const String & column_name) const;
    bool hasNotAlias(const String & column_name) const;
    bool hasAlias(const String & column_name) const;
    bool hasColumnOrSubcolumn(GetColumnsOptions::Kind kind, const String & column_name) const;
    bool hasColumnOrNested(GetColumnsOptions::Kind kind, const String & column_name) const;

    bool hasOnlyOrdinary() const;

    NameAndTypePair getPhysical(const String & column_name) const;
    NameAndTypePair getColumnOrSubcolumn(GetColumnsOptions::Kind kind, const String & column_name) const;
    NameAndTypePair getColumn(const GetColumnsOptions & options, const String & column_name) const;

    std::optional<NameAndTypePair> tryGetPhysical(const String & column_name) const;
    std::optional<NameAndTypePair> tryGetColumnOrSubcolumn(GetColumnsOptions::Kind kind, const String & column_name) const;
    std::optional<NameAndTypePair> tryGetColumn(const GetColumnsOptions & options, const String & column_name) const;

    std::optional<const ColumnDescription> tryGetColumnOrSubcolumnDescription(GetColumnsOptions::Kind kind, const String & column_name) const;
    std::optional<const ColumnDescription> tryGetColumnDescription(const GetColumnsOptions & options, const String & column_name) const;

    ColumnDefaults getDefaults() const;
    bool hasDefault(const String & column_name) const;
    bool hasDefaults() const;
    std::optional<ColumnDefault> getDefault(const String & column_name) const;

    /// Does column has non default specified compression codec
    bool hasCompressionCodec(const String & column_name) const;

    String toString(bool include_comments) const;
    static ColumnsDescription parse(const String & str);

    size_t size() const
    {
        return columns.size();
    }

    bool empty() const
    {
        return columns.empty();
    }

    std::vector<String> getAllRegisteredNames() const override;

    /// Keep the sequence of columns and allow to lookup by name.
    using ColumnsContainer = boost::multi_index_container<
        ColumnDescription,
        boost::multi_index::indexed_by<
            boost::multi_index::sequenced<>,
            boost::multi_index::ordered_unique<boost::multi_index::member<ColumnDescription, String, &ColumnDescription::name>>>>;

    using SubcolumnsContainter = boost::multi_index_container<
        NameAndTypePair,
        boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<boost::multi_index::member<NameAndTypePair, String, &NameAndTypePair::name>>,
            boost::multi_index::hashed_non_unique<boost::multi_index::const_mem_fun<NameAndTypePair, String, &NameAndTypePair::getNameInStorage>>>>;

private:
    ColumnsContainer columns;

    /// Subcolumns are not nested columns.
    ///
    /// Example of subcolumns:
    /// - .size0 for Array
    /// - .null  for Nullable
    ///
    /// While nested columns have form like foo.bar
    SubcolumnsContainter subcolumns;

    void modifyColumnOrder(const String & column_name, const String & after_column, bool first);
    void addSubcolumnsToList(NamesAndTypesList & source_list) const;

    void addSubcolumns(const String & name_in_storage, const DataTypePtr & type_in_storage);
    void removeSubcolumns(const String & name_in_storage);
};

class ASTColumnDeclaration;

struct DefaultExpressionsInfo
{
    ASTPtr expr_list = nullptr;
    bool has_columns_with_default_without_type = false;
};

void getDefaultExpressionInfoInto(const ASTColumnDeclaration & col_decl, const DataTypePtr & data_type, DefaultExpressionsInfo & info);

/// Validate default expressions and corresponding types compatibility, i.e.
/// default expression result can be cast to column_type. Also checks, that we
/// don't have strange constructions in default expression like SELECT query or
/// arrayJoin function.
void validateColumnsDefaults(ASTPtr default_expr_list, const NamesAndTypesList & all_columns, ContextPtr context);
Block validateColumnsDefaultsAndGetSampleBlock(ASTPtr default_expr_list, const NamesAndTypesList & all_columns, ContextPtr context);

}
