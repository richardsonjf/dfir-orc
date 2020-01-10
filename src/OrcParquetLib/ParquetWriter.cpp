//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//
#include "stdafx.h"

#include "ParquetWriter.h"

#include "FileStream.h"
#include "ParquetStream.h"

#include "Robustness.h"
#include "OrcException.h"
#include "WideAnsi.h"
#include "Buffer.h"

#include <string>
#include <string_view>

#include "ParquetDefinitions.h"

using namespace Orc;

class Orc::TableOutput::Parquet::WriterTermination : public TerminationHandler
{
public:
    WriterTermination(const std::wstring& strDescr, std::weak_ptr<Writer> pW)
        : TerminationHandler(strDescr, ROBUSTNESS_CSV)
        , m_pWriter(std::move(pW)) {};

    HRESULT operator()();

private:
    std::weak_ptr<Writer> m_pWriter;
};

HRESULT Orc::TableOutput::Parquet::WriterTermination::operator()()
{
    if (auto pWriter = m_pWriter.lock(); pWriter)
    {
        pWriter->Flush();
    }
    return S_OK;
}

struct Orc::TableOutput::Parquet::Writer::MakeSharedEnabler : public Orc::TableOutput::Parquet::Writer
{
    MakeSharedEnabler(logger pLog, std::unique_ptr<Options>&& options)
        : Writer(std::move(pLog), std::move(options))
    {
    }
};

std::shared_ptr<Orc::TableOutput::Parquet::Writer>
Orc::TableOutput::Parquet::Writer::MakeNew(logger pLog, std::unique_ptr<Options>&& options)
{
    auto retval = std::make_shared<MakeSharedEnabler>(std::move(pLog), std::move(options));

    std::wstring strDescr = L"Termination for ParquetWriter";
    retval->m_pTermination = std::make_shared<WriterTermination>(strDescr, retval);
    Robustness::AddTerminationHandler(retval->m_pTermination);
    return retval;
}

Orc::TableOutput::Parquet::Writer::Writer(logger pLog, std::unique_ptr<Options>&& options)
    : _L_(std::move(pLog))
    , m_Options(std::move(options))
{
}

Orc::TableOutput::Parquet::Writer::Builders Orc::TableOutput::Parquet::Writer::GetBuilders()
{
    Builders retval;
    retval.reserve(m_Schema.size());

    auto pool = arrow::default_memory_pool();

    for (const auto& column : m_arrowSchema->fields())
    {
        switch (column->type()->id())
        {
            case arrow::Type::NA:
            {
                retval.emplace_back(std::make_unique<arrow::NullBuilder>(pool));
                break;
            }
            case arrow::Type::BOOL:
            {
                retval.emplace_back(std::make_unique<arrow::BooleanBuilder>(column->type(), pool));
                break;
            }
            case arrow::Type::UINT8:
            {
                retval.emplace_back(std::make_unique<arrow::UInt8Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::INT8:
            {
                retval.emplace_back(std::make_unique<arrow::Int8Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::UINT16:
            {
                retval.emplace_back(std::make_unique<arrow::UInt16Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::INT16:
            {
                retval.emplace_back(std::make_unique<arrow::Int16Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::UINT32:
            {
                retval.emplace_back(std::make_unique<arrow::UInt32Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::INT32:
            {
                retval.emplace_back(std::make_unique<arrow::Int32Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::UINT64:
            {
                retval.emplace_back(std::make_unique<arrow::UInt64Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::INT64:
            {
                retval.emplace_back(std::make_unique<arrow::Int64Builder>(column->type(), pool));
                break;
            }
            case arrow::Type::TIMESTAMP:
            {
                retval.emplace_back(std::make_unique<arrow::TimestampBuilder>(column->type(), pool));
                break;
            }
            case arrow::Type::STRING:
            {
                retval.emplace_back(std::make_unique<arrow::StringBuilder>(column->type(), pool));
                break;
            }
            case arrow::Type::BINARY:
            {
                retval.emplace_back(std::make_unique<arrow::BinaryBuilder>(column->type(), pool));
                break;
            }
            case arrow::Type::FIXED_SIZE_BINARY:
            {
                retval.emplace_back(std::make_unique<arrow::FixedSizeBinaryBuilder>(column->type(), pool));
                break;
            }
            case arrow::Type::DICTIONARY:
            {
                retval.emplace_back(std::make_unique<arrow::StringDictionaryBuilder>(column->type(), pool));
                break;
            }
            case arrow::Type::LIST:
            {
                std::unique_ptr<arrow::ArrayBuilder> value_builder;
                std::shared_ptr<arrow::DataType> value_type =
                    dynamic_cast<const arrow::ListType&>(*column->type()).value_type();
                auto status = arrow::MakeBuilder(pool, value_type, &value_builder);
                retval.emplace_back(std::make_unique<arrow::ListBuilder>(pool, std::move(value_builder)));
                break;
            }
            case arrow::Type::STRUCT:
            {
                const std::vector<std::shared_ptr<arrow::Field>>& fields = column->type()->children();
                std::vector<std::shared_ptr<arrow::ArrayBuilder>> values_builder;

                for (const auto& it : fields)
                {

                    std::unique_ptr<arrow::ArrayBuilder> builder;

                    auto status = arrow::MakeBuilder(pool, it->type(), &builder);
                    if (status.ok())
                        values_builder.emplace_back(std::move(builder));
                    else
                    {
                        throw Orc::Exception(Fatal, E_POINTER, L"Failed to create builder for field\r\n");
                    }
                }
                retval.emplace_back(
                    std::make_unique<arrow::StructBuilder>(column->type(), pool, std::move(values_builder)));
                break;
            }
            case arrow::Type::MAP:
            {
                break;
            }
            default:
            {
                throw Orc::Exception(
                    Fatal,
                    E_INVALIDARG,
                    L"Failed to create builder for type %S\r\n",
                    column->type()->ToString().c_str());
            }
        }
    }
    return retval;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::SetSchema(const TableOutput::Schema& columns)
{
    if (!columns)
        return E_INVALIDARG;

    m_Schema = columns;
    m_dwColumnNumber = static_cast<DWORD>(m_Schema.size());

    parquet::WriterProperties::Builder props_builder;
    props_builder.data_pagesize(4096 * 1024);
    props_builder.max_row_group_length(10000);
    props_builder.compression(parquet::Compression::GZIP);

    m_parquetProps = props_builder.build();

    std::vector<std::shared_ptr<arrow::Field>> schema_definition;
    schema_definition.reserve(columns.size());

    m_arrowBuilders.reserve(m_Schema.size());

    for (const auto& column : m_Schema)
    {
        auto [hr, strName] = WideToAnsi(_L_, column->ColumnName);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Invalid column name %s", column->ColumnName.c_str());
            break;
        }

        switch (column->Type)
        {
            case Nothing:
                schema_definition.push_back(arrow::field(strName, arrow::null(), true));
                break;
            case BoolType:
                schema_definition.push_back(arrow::field(strName, arrow::boolean(), true));
                break;
            case UInt8Type:
                schema_definition.push_back(arrow::field(strName, arrow::uint8(), true));
                break;
            case Int8Type:
                schema_definition.push_back(arrow::field(strName, arrow::int8(), true));
                break;
            case UInt16Type:
                schema_definition.push_back(arrow::field(strName, arrow::uint16(), true));
                break;
            case Int16Type:
                schema_definition.push_back(arrow::field(strName, arrow::int16(), true));
                break;
            case UInt32Type:
                schema_definition.push_back(arrow::field(strName, arrow::uint32(), true));
                break;
            case Int32Type:
                schema_definition.push_back(arrow::field(strName, arrow::int32(), true));
                break;
            case UInt64Type:
                schema_definition.push_back(arrow::field(strName, arrow::uint64(), true));
                break;
            case Int64Type:
                schema_definition.push_back(arrow::field(strName, arrow::int64(), true));
                break;
            case TimeStampType:
                schema_definition.push_back(arrow::field(strName, arrow::timestamp(arrow::TimeUnit::MICRO), true));
                break;
            case UTF16Type:
                schema_definition.push_back(arrow::field(strName, arrow::binary(), true));
                break;
            case UTF8Type:
                schema_definition.push_back(arrow::field(strName, arrow::utf8(), true));
                break;
            case BinaryType:
                schema_definition.push_back(arrow::field(strName, arrow::binary(), true));
                break;
            case FixedBinaryType:
                schema_definition.push_back(
                    arrow::field(strName, arrow::fixed_size_binary(column->dwLen.value()), true));
                break;
            case GUIDType:
                schema_definition.push_back(arrow::field(strName, arrow::fixed_size_binary(16), true));
                break;
            case EnumType:
            {
                std::shared_ptr<arrow::KeyValueMetadata> metadata;

                if (column->EnumValues.has_value())
                {
                    metadata = std::make_shared<arrow::KeyValueMetadata>();
                    for (const auto& value : column->EnumValues.value())
                    {
                        auto index = fmt::format("{:#08x}", value.Index);

                        if (auto [hr, str] = WideToAnsi(_L_, value.strValue); SUCCEEDED(hr))
                            metadata->Append(index, str);
                    }
                }

                schema_definition.push_back(arrow::field(strName, arrow::uint32(), true, metadata));
                break;
            }
            case XMLType:
                schema_definition.push_back(arrow::field(strName, arrow::binary(), true));
                break;
            case FlagsType:
            {
                std::shared_ptr<arrow::KeyValueMetadata> metadata;

                if (column->FlagsValues.has_value())
                {
                    metadata = std::make_shared<arrow::KeyValueMetadata>();
                    for (const auto& value : column->FlagsValues.value())
                    {
                        auto index = fmt::format("{:#08x}", value.dwFlag);

                        if (auto [hr, str] = WideToAnsi(_L_, value.strFlag); SUCCEEDED(hr))
                            metadata->Append(index, str);
                    }
                }

                schema_definition.push_back(arrow::field(strName, arrow::uint32(), true, metadata));
                break;
            }
            default:
                log::Error(_L_, E_FAIL, L"Unupported (parquet) column type for column %s", column->ColumnName.c_str());
                return E_FAIL;
        }
    }
    m_arrowSchema = std::make_shared<arrow::Schema>(schema_definition);
    m_arrowBuilders = GetBuilders();
    return S_OK;
}

HRESULT Orc::TableOutput::Parquet::Writer::WriteToFile(const WCHAR* szFileName)
{
    HRESULT hr = E_FAIL;

    if (szFileName == NULL)
        return E_POINTER;

    auto pFileStream = std::make_shared<FileStream>(_L_);

    if (pFileStream == nullptr)
        return E_OUTOFMEMORY;

    // ... create it
    if (FAILED(hr = pFileStream->WriteTo(szFileName)))  // no attr.	template
        return hr;

    return WriteToStream(pFileStream, true);
}

STDMETHODIMP
Orc::TableOutput::Parquet::Writer::WriteToStream(const std::shared_ptr<ByteStream>& pStream, bool bCloseStream)
{
    HRESULT hr = E_FAIL;

    if (m_pByteStream && m_bCloseStream)
    {
        m_pByteStream->Close();
    }

    m_bCloseStream = bCloseStream;
    m_pByteStream = pStream;

    auto arrow_output_stream = std::make_shared<Orc::TableOutput::Parquet::Stream>(_L_);

    if (auto hr = arrow_output_stream->Open(pStream); FAILED(hr))
        return hr;

    if (m_arrowSchema->num_fields() == 0 || m_arrowBuilders.size() == 0)
    {
        log::Error(_L_, E_FAIL, L"Cannot write to a parquet file without a schema");
        return E_FAIL;
    }

    parquet::arrow::FileWriter::Open(
        *m_arrowSchema,
        ::arrow::default_memory_pool(),
        arrow_output_stream,
        m_parquetProps,
        parquet::default_arrow_writer_properties(),
        &m_arrowWriter);

    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::Flush()
{
    ScopedLock sl(m_cs);

    log::Verbose(_L_, L"Orc::TableOutput::Parquet::Writer::Flush");

    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(m_arrowBuilders.size());

    for (const auto& builder : m_arrowBuilders)
    {
        std::visit(
            [&arrays](auto&& arg) {
                std::shared_ptr<arrow::Array> column_array;

                arg->Finish(&column_array);

                arrays.emplace_back(std::move(column_array));
            },
            builder);
    }

    auto table = arrow::Table::Make(m_arrowSchema, arrays);
    if (!table)
    {
        log::Error(_L_, E_FAIL, L"Failed to create arrow table (to flush)\r\n");
        return E_FAIL;
    }

    auto status = m_arrowWriter->WriteTable(*table, table->num_rows());
    if (!status.ok())
    {
        log::Error(_L_, E_FAIL, L"Failed to write arrow table (%S)\r\n", status.ToString().c_str());
        return E_FAIL;
    }

    m_arrowBuilders = GetBuilders();

    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::Close()
{

    if (auto hr = Flush(); FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to flush arrow table\r\n");
        return hr;
    }

    if (m_pTermination)
    {
        ScopedLock sl(m_cs);
        Robustness::RemoveTerminationHandler(m_pTermination);
        m_pTermination = nullptr;
    }
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::AddColumnAndCheckNumbers()
{
    m_dwColumnCounter++;
    if (m_dwColumnCounter > m_dwColumnNumber)
    {
        auto counter = m_dwColumnCounter;
        m_dwColumnCounter = 0L;
        throw Orc::Exception(
            ExceptionSeverity::Fatal,
            L"Too many columns written to Parquet (got %d, max is %d)",
            counter,
            m_dwColumnNumber);
    }
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteNothing()
{
    std::visit(
        [](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (!std::is_same_v<T, std::unique_ptr<arrow::ArrayBuilder>>)
                arg->AppendNull();
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Cannot append WriteNothing via Array Builder");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::AbandonRow()
{
    for (auto i = m_dwColumnCounter; i < m_dwColumnNumber; i++)
    {
        std::visit(
            [](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (!std::is_same_v<T, std::unique_ptr<arrow::ArrayBuilder>>)
                    arg->AppendNull();
                else
                    throw Orc::Exception(ExceptionSeverity::Fatal, L"Cannot append AbandonRow via Array Builder");
            },
            m_arrowBuilders[i]);
        AddColumnAndCheckNumbers();
    }
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::AbandonColumn()
{
    return WriteNothing();
}

HRESULT Orc::TableOutput::Parquet::Writer::WriteEndOfLine()
{
    if (m_dwColumnCounter == m_dwColumnNumber)
    {
        m_dwColumnCounter = 0L;
    }
    else if (m_dwColumnCounter < m_dwColumnNumber)
    {
        auto counter = m_dwColumnCounter;
        m_dwColumnCounter = 0L;
        throw Orc::Exception(
            ExceptionSeverity::Fatal,
            L"Too few columns written to Parquet (got %d, max is %d)",
            counter,
            m_dwColumnNumber);
    }
    else if (m_dwColumnCounter > m_dwColumnNumber)
    {
        auto counter = m_dwColumnCounter;
        m_dwColumnCounter = 0L;
        throw Orc::Exception(
            ExceptionSeverity::Fatal,
            L"Too many columns written to Parquet (got %d, max is %d)",
            counter,
            m_dwColumnNumber);
    }
    m_dwBatchRowCount++;
    m_dwTotalRowCount++;

    if (m_Options && m_Options->BatchSize.has_value())
    {
        if (m_dwBatchRowCount >= m_Options->BatchSize.value())
        {
            log::Verbose(_L_, L"Batch is full --> Flush() (%d rows)", m_dwBatchRowCount);
            if (auto hr = Flush(); FAILED(hr))
                return hr;
        }
    }
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteString(const std::wstring& strString)
{
    std::visit(
        [this, &strString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(
                    reinterpret_cast<const uint8_t* const>(strString.data()),
                    (uint32_t)strString.size() * sizeof(WCHAR));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
            {
                if (auto [hr, utf8] = WideToAnsi(_L_, strString); SUCCEEDED(hr))
                {
                    arg->Append(utf8);
                }
                else
                    arg->AppendNull();
            }
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a Unicode string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteString(const std::wstring_view& strString)
{
    std::visit(
        [this, &strString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(
                    reinterpret_cast<const uint8_t* const>(strString.data()),
                    (uint32_t)strString.size() * sizeof(WCHAR));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
            {
                if (auto [hr, utf8] = WideToAnsi(_L_, strString); SUCCEEDED(hr))
                {
                    arg->Append(utf8);
                }
                else
                    arg->AppendNull();
            }
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a Unicode string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteString(const WCHAR* szString)
{
    std::visit(
        [this, szString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(
                    reinterpret_cast<const uint8_t* const>(szString), (uint32_t)wcslen(szString) * sizeof(WCHAR));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
            {
                if (auto [hr, utf8] = WideToAnsi(_L_, szString); SUCCEEDED(hr))
                {
                    arg->Append(utf8);
                }
                else
                    arg->AppendNull();
            }
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a Unicode string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteCharArray(const WCHAR* szString, DWORD dwCharCount)
{
    std::visit(
        [this, szString, dwCharCount](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(reinterpret_cast<const uint8_t* const>(szString), (uint32_t)dwCharCount * sizeof(WCHAR));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
            {
                if (auto [hr, utf8] = WideToAnsi(_L_, std::wstring_view(szString, dwCharCount)); SUCCEEDED(hr))
                {
                    arg->Append(utf8);
                }
                else
                    arg->AppendNull();
            }
            else
                throw Orc::Exception(Fatal, L"Not a valid arrow builder for a Unicode string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP
Orc::TableOutput::Parquet::Writer::WriteFormated_(const std::wstring_view& szFormat, IOutput::wformat_args args)
{
    Buffer<WCHAR, MAX_PATH> buffer;

    auto result = fmt::vformat_to(std::back_inserter(buffer), szFormat, args);

    if (buffer.empty())
        return WriteNothing();
    else
        return WriteCharArray(buffer.get(), buffer.size());
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteString(const std::string& strString)
{
    std::visit(
        [&strString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
                arg->Append(strString);
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an ANSI string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteString(const std::string_view& strString)
{
    std::visit(
        [&strString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
                arg->Append(strString.data(), strString.size());
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an ANSI string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteString(const CHAR* szString)
{
    std::visit(
        [szString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
                arg->Append(szString, static_cast<uint32_t>(strlen(szString)));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an ANSI string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteCharArray(const CHAR* szString, DWORD dwCharCount)
{
    std::visit(
        [szString, dwCharCount](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::StringBuilder>>)
                arg->Append(szString, static_cast<uint32_t>(dwCharCount));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an ANSI string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP
Orc::TableOutput::Parquet::Writer::WriteFormated_(const std::string_view& szFormat, IOutput::format_args args)
{
    Buffer<CHAR, MAX_PATH> buffer;

    auto result = fmt::vformat_to(std::back_inserter(buffer), szFormat, args);

    if (buffer.empty())
        return WriteNothing();
    else
        return WriteCharArray(buffer.get(), buffer.size());
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteAttributes(DWORD dwFileAttributes)
{
    if (auto hr = WriteFormated(
            "{}{}{}{}{}{}{}{}{}{}{}{}{}",
            dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE ? 'A' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED ? 'C' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 'D' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED ? 'E' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_HIDDEN ? 'H' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_NORMAL ? 'N' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_OFFLINE ? 'O' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_READONLY ? 'R' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? 'L' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE ? 'P' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_SYSTEM ? 'S' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY ? 'T' : '.',
            dwFileAttributes & FILE_ATTRIBUTE_VIRTUAL ? 'V' : '.');
        FAILED(hr))
    {
        AbandonColumn();
        return hr;
    }
    return S_OK;
}

HRESULT Orc::TableOutput::Parquet::Writer::WriteFileTime(FILETIME fileTime)
{
    std::visit(
        [fileTime](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::TimestampBuilder>>)
                arg->Append(ConvertTo(fileTime));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an FILETIME value");
        },
        m_arrowBuilders[m_dwColumnCounter]);

    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteFileTime(LONGLONG fileTime)
{
    std::visit(
        [fileTime](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::TimestampBuilder>>)
                arg->Append(fileTime);
            else
                throw Orc::Exception(
                    ExceptionSeverity::Fatal, L"Not a valid arrow builder for an LONGLONG fileTime value");
        },
        m_arrowBuilders[m_dwColumnCounter]);

    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteTimeStamp(time_t tmStamp)
{
    std::visit(
        [tmStamp](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::TimestampBuilder>>)
                arg->Append(ConvertTo(tmStamp));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an time_t value");
        },
        m_arrowBuilders[m_dwColumnCounter]);

    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteTimeStamp(tm tmStamp)
{
    std::visit(
        [tmStamp](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::TimestampBuilder>>)
                arg->Append(ConvertTo(tmStamp));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an time_t value");
        },
        m_arrowBuilders[m_dwColumnCounter]);

    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteFileSize(LARGE_INTEGER fileSize)
{
    std::visit(
        [fileSize](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt64Builder>>)
                arg->Append(fileSize.QuadPart);
            else
                throw Orc::Exception(
                    ExceptionSeverity::Fatal, L"Not a valid arrow builder for an LARGE_INTEGER FileSize value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteFileSize(ULONGLONG fileSize)
{
    std::visit(
        [fileSize](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt64Builder>>)
                arg->Append(fileSize);
            else
                throw Orc::Exception(
                    ExceptionSeverity::Fatal, L"Not a valid arrow builder for an ULONGLONG FileSize value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteFileSize(DWORD nFileSizeHigh, DWORD nFileSizeLow)
{
    LARGE_INTEGER FileSize;

    FileSize.HighPart = nFileSizeHigh;
    FileSize.LowPart = nFileSizeLow;

    // File	size calculation only required for Very	big	files.
    return WriteFileSize(FileSize);
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteBool(bool bBoolean)
{
    std::visit(
        [bBoolean](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BooleanBuilder>>)
                arg->Append(bBoolean);
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a boolean value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteEnum(DWORD dwEnum)
{
    std::visit(
        [dwEnum](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt32Builder>>)
                arg->Append(static_cast<uint32_t>(dwEnum));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int32Builder>>)
                arg->Append(static_cast<int32_t>(dwEnum));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an enum value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}
STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteEnum(DWORD dwEnum, const WCHAR* EnumValues[])
{
    return WriteEnum(dwEnum);
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteFlags(DWORD dwFlags)
{
    std::visit(
        [dwFlags](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt32Builder>>)
                arg->Append(static_cast<uint64_t>(dwFlags));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int32Builder>>)
                arg->Append(static_cast<int64_t>(dwFlags));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an enum value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP
Orc::TableOutput::Parquet::Writer::WriteFlags(DWORD dwFlags, const FlagsDefinition FlagValues[], WCHAR cSeparator)
{
    return WriteFlags(dwFlags);
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteExactFlags(DWORD dwFlags)
{
    std::visit(
        [dwFlags](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt32Builder>>)
                arg->Append(static_cast<uint64_t>(dwFlags));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int32Builder>>)
                arg->Append(static_cast<int64_t>(dwFlags));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an enum value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteExactFlags(DWORD dwFlags, const FlagsDefinition FlagValues[])
{
    return WriteExactFlags(dwFlags);
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteGUID(const GUID& guid)
{
    HRESULT hr = E_FAIL;
    std::visit(
        [guid](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::FixedSizeBinaryBuilder>>)
                arg->Append(reinterpret_cast<const uint8_t*>(&guid));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a GUID value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteXML(const WCHAR* szString)
{
    std::visit(
        [szString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(
                    reinterpret_cast<const uint8_t* const>(szString), (uint32_t)wcslen(szString) * sizeof(WCHAR));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a XML string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteXML(const WCHAR* szString, DWORD dwCharCount)
{
    std::visit(
        [szString, dwCharCount](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(reinterpret_cast<const uint8_t* const>(szString), dwCharCount * sizeof(WCHAR));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a XML string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteXML(const CHAR* szString)
{
    std::visit(
        [szString](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(reinterpret_cast<const uint8_t* const>(szString), static_cast<int32_t>(strlen(szString)));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a XML string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteXML(const CHAR* szString, DWORD dwCharCount)
{
    std::visit(
        [szString, dwCharCount](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(reinterpret_cast<const uint8_t* const>(szString), dwCharCount);
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for a XML string");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteInteger(DWORD dwInteger)
{
    auto result = Orc::ConvertTo<DWORD>(dwInteger);

    std::visit(
        [dwInteger](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt32Builder>>)
                arg->Append(dwInteger);
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int32Builder>>)
                arg->Append(static_cast<int32_t>(dwInteger));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt64Builder>>)
                arg->Append(static_cast<uint64_t>(dwInteger));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int64Builder>>)
                arg->Append(static_cast<int64_t>(dwInteger));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an DWORD value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteInteger(LONGLONG llInteger)
{
    std::visit(
        [llInteger](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt64Builder>>)
                arg->Append(static_cast<uint64_t>(llInteger));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int64Builder>>)
                arg->Append(static_cast<int64_t>(llInteger));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an LONGLONG value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteInteger(ULONGLONG ullInteger)
{
    std::visit(
        [ullInteger](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::UInt64Builder>>)
                arg->Append(static_cast<uint64_t>(ullInteger));
            else if constexpr (std::is_same_v<T, std::unique_ptr<arrow::Int64Builder>>)
                arg->Append(static_cast<int64_t>(ullInteger));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an ULONGLONG value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteBytes(const BYTE pBytes[], DWORD dwLen)
{
    std::visit(
        [pBytes, dwLen](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<arrow::BinaryBuilder>>)
                arg->Append(reinterpret_cast<const uint8_t* const>(&pBytes), sizeof(dwLen));
            else
                throw Orc::Exception(ExceptionSeverity::Fatal, L"Not a valid arrow builder for an LONGLONG hex value");
        },
        m_arrowBuilders[m_dwColumnCounter]);
    AddColumnAndCheckNumbers();
    return S_OK;
}

STDMETHODIMP Orc::TableOutput::Parquet::Writer::WriteBytes(const CBinaryBuffer& Buffer)
{
    return WriteBytes(Buffer.GetData(), (DWORD)Buffer.GetCount());
}

Orc::TableOutput::Parquet::Writer::~Writer(void)
{
    Close();
}
