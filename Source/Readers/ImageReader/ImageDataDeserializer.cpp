//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <opencv2/opencv.hpp>
#include <numeric>
#include <limits>
#include "ImageDataDeserializer.h"
#include "ImageConfigHelper.h"
#include "StringUtil.h"
#include "ConfigUtil.h"
#include "TimerUtility.h"

namespace Microsoft { namespace MSR { namespace CNTK {

class ImageDataDeserializer::LabelGenerator
{
public:
    virtual void CreateLabelFor(size_t classId, SparseSequenceData& data) = 0;
    virtual ~LabelGenerator() { }
};

// A helper class to generate a typed label in a sparse format.
// A label is just a category/class the image belongs to.
// It is represented as a array indexed by the category with zero values for all categories the image does not belong to, 
// and a single one for a category it belongs to: [ 0, .. 0.. 1 .. 0 ]
// The class is parameterized because the representation of 1 is type specific.
template <class TElement>
class TypedLabelGenerator : public ImageDataDeserializer::LabelGenerator
{
public:
    TypedLabelGenerator(size_t labelDimension) : m_value(1), m_indices(labelDimension)
    {
        if (labelDimension > numeric_limits<IndexType>::max())
        {
            RuntimeError("Label dimension (%" PRIu64 ") exceeds the maximum allowed "
                "value (%" PRIu64 ")\n", labelDimension, (size_t)numeric_limits<IndexType>::max());
        }
        iota(m_indices.begin(), m_indices.end(), 0);
    }

    virtual void CreateLabelFor(size_t classId, SparseSequenceData& data) override
    {
        data.m_nnzCounts.resize(1);
        data.m_nnzCounts[0] = 1;
        data.m_totalNnzCount = 1;
        data.m_data = &m_value;
        data.m_indices = &(m_indices[classId]);
    }

private:
    TElement m_value;
    vector<IndexType> m_indices;
};

// Used to keep track of the image. Accessed only using DenseSequenceData interface.
struct DeserializedImage : DenseSequenceData
{
    cv::Mat m_image;
    ImageDataPtr m_buffer;
};

// For image, chunks correspond to a single image.
class ImageDataDeserializer::ImageChunk : public Chunk, public std::enable_shared_from_this<ImageChunk>
{
    //ImageSequenceDescription m_description;
    ChunkIdType m_id;
    ImageDataDeserializer& m_parent;

    std::vector<ImageDataPtr> m_data;

public:
    ImageChunk(ChunkIdType id, ImageDataDeserializer& parent)
        : m_id(id), m_parent(parent)
    {
        // Let's read all sequences we need.
        const auto& currentChunk = *m_parent.m_chunks[m_id];
        m_data.reserve(currentChunk.m_numberOfSamples);
        for (int i = 0; i < currentChunk.m_numberOfSamples; ++i)
        {
            size_t currentImage = currentChunk.m_startIndex + i;
            const auto& imageSequence = m_parent.m_imageSequences[currentImage];

            auto read = m_parent.ReadImage(imageSequence.m_id, imageSequence.m_path);
            m_data.push_back(read);
        }
    }

    virtual void GetSequence(size_t sequenceId, std::vector<SequenceDataPtr>& result) override
    {
        const auto& currentChunk = *m_parent.m_chunks[m_id];
        size_t index = sequenceId - currentChunk.m_startIndex;

        const auto& imageSequence = m_parent.m_imageSequences[sequenceId];

        auto image = std::make_shared<DeserializedImage>();

        auto data = m_data[index];
        image->m_image = cv::imdecode(cv::Mat(1, (int)data->m_size, CV_8UC1, data->m_data), m_parent.m_grayscale ? cv::IMREAD_GRAYSCALE : cv::IMREAD_COLOR);

        auto& cvImage = image->m_image;
        if (!cvImage.data)
        {
            RuntimeError("Cannot open file '%s'", imageSequence.m_path.c_str());
        }

        // Convert element type.
        ElementType elementType = m_parent.m_featureElementType;
        auto depth =  cvImage.type() & CV_MAT_DEPTH_MASK;
        if (depth == CV_32F)
        {
            elementType = ElementType::tfloat;
        }
        else if (depth == CV_64F)
        {
            elementType = ElementType::tdouble;
        }
        else if (depth == CV_8U)
        {
            elementType = ElementType::tuchar;
        }
        else
        {
            // Heavy operation, have to convert.
            cvImage.convertTo(cvImage, elementType == ElementType::tfloat ? CV_32F : CV_64F);
        }

        if (!cvImage.isContinuous())
        {
            cvImage = cvImage.clone();
        }
        assert(cvImage.isContinuous());

        image->m_data = image->m_image.data;
        ImageDimensions dimensions(cvImage.cols, cvImage.rows, cvImage.channels());
        image->m_sampleLayout = std::make_shared<TensorShape>(dimensions.AsTensorShape(HWC));
        image->m_id = imageSequence.m_id;
        image->m_numberOfSamples = 1;
        image->m_chunk = shared_from_this();
        image->m_buffer = data;
        image->m_elementType = elementType;

        SparseSequenceDataPtr label = std::make_shared<SparseSequenceData>();
        label->m_chunk = shared_from_this();
        m_parent.m_labelGenerator->CreateLabelFor(imageSequence.m_classId, *label);
        label->m_numberOfSamples = 1;
        label->m_elementType = m_parent.m_labelElementType;

        result.push_back(image);
        result.push_back(label);
    }
};

// A new constructor to support new compositional configuration,
// that allows composition of deserializers and transforms on inputs.
ImageDataDeserializer::ImageDataDeserializer(CorpusDescriptorPtr corpus, const ConfigParameters& config)
{
    ConfigParameters inputs = config("input");
    std::vector<std::string> featureNames = GetSectionsWithParameter("ImageDataDeserializer", inputs, "transforms");
    std::vector<std::string> labelNames = GetSectionsWithParameter("ImageDataDeserializer", inputs, "labelDim");

    // TODO: currently support only one feature and label section.
    if (featureNames.size() != 1 || labelNames.size() != 1)
    {
        RuntimeError(
            "ImageReader currently supports a single feature and label stream. '%d' features , '%d' labels found.",
            static_cast<int>(featureNames.size()),
            static_cast<int>(labelNames.size()));
    }

    string precision = (ConfigValue)config("precision", "float");
    m_verbosity = config(L"verbosity", 0);

    // Feature stream.
    ConfigParameters featureSection = inputs(featureNames[0]);
    auto features = std::make_shared<StreamDescription>();
    features->m_id = 0;
    features->m_name = msra::strfun::utf16(featureSection.ConfigName());
    features->m_storageType = StorageType::dense;
    features->m_elementType = ElementType::tend; // Each image can have its own.
    m_streams.push_back(features);
    m_featureElementType = features->m_elementType;

    // Label stream.
    ConfigParameters label = inputs(labelNames[0]);
    size_t labelDimension = label("labelDim");
    auto labels = std::make_shared<StreamDescription>();
    labels->m_id = 1;
    labels->m_name = msra::strfun::utf16(label.ConfigName());
    labels->m_sampleLayout = std::make_shared<TensorShape>(labelDimension);
    labels->m_storageType = StorageType::sparse_csc;
    labels->m_elementType = AreEqualIgnoreCase(precision, "float") ? ElementType::tfloat : ElementType::tdouble;
    m_labelElementType = labels->m_elementType;
    m_streams.push_back(labels);

    m_labelGenerator = labels->m_elementType == ElementType::tfloat ?
        (LabelGeneratorPtr)std::make_shared<TypedLabelGenerator<float>>(labelDimension) :
        std::make_shared<TypedLabelGenerator<double>>(labelDimension);

    m_grayscale = config(L"grayscale", false);

    // TODO: multiview should be done on the level of randomizer/transformers - it is responsiblity of the
    // TODO: randomizer to collect how many copies each transform needs and request same sequence several times.
    bool multiViewCrop = config(L"multiViewCrop", false);
    CreateSequenceDescriptions(corpus, config(L"file"), labelDimension, multiViewCrop);
}

// TODO: Should be removed at some point.
// Supports old type of ImageReader configuration.
ImageDataDeserializer::ImageDataDeserializer(const ConfigParameters& config)
{
    ImageConfigHelper configHelper(config);
    m_streams = configHelper.GetStreams();
    assert(m_streams.size() == 2);
    m_grayscale = configHelper.UseGrayscale();
    const auto& label = m_streams[configHelper.GetLabelStreamId()];
    const auto& feature = m_streams[configHelper.GetFeatureStreamId()];

    m_verbosity = config(L"verbosity", 0);

    // Expect data in HWC.
    ImageDimensions dimensions(*feature->m_sampleLayout, configHelper.GetDataFormat());
    feature->m_sampleLayout = std::make_shared<TensorShape>(dimensions.AsTensorShape(HWC));

    label->m_storageType = StorageType::sparse_csc;
    feature->m_storageType = StorageType::dense;

    m_featureElementType = feature->m_elementType;
    feature->m_elementType = ElementType::tend;
    size_t labelDimension = label->m_sampleLayout->GetDim(0);

    if (label->m_elementType == ElementType::tfloat)
    {
        m_labelGenerator = std::make_shared<TypedLabelGenerator<float>>(labelDimension);
    }
    else if (label->m_elementType == ElementType::tdouble)
    {
        m_labelGenerator = std::make_shared<TypedLabelGenerator<double>>(labelDimension);
    }
    else
    {
        RuntimeError("Unsupported label element type '%d'.", (int)label->m_elementType);
    }

    CreateSequenceDescriptions(std::make_shared<CorpusDescriptor>(), configHelper.GetMapPath(), labelDimension, configHelper.IsMultiViewCrop());
}

// Descriptions of chunks exposed by the image reader.
ChunkDescriptions ImageDataDeserializer::GetChunkDescriptions()
{
    ChunkDescriptions result;
    result.reserve(m_chunks.size());
    for (const auto& c : m_chunks)
    {
        result.push_back(c);
    }
    return result;
}

void ImageDataDeserializer::GetSequencesForChunk(ChunkIdType chunkId, std::vector<SequenceDescription>& result)
{
    // Currently a single sequence per chunk.
    for (size_t i = 0; i < m_chunks[chunkId]->m_numberOfSequences; i++)
    {
        result.push_back(m_imageSequences[m_chunks[chunkId]->m_startIndex + i]);
    }
}

void ImageDataDeserializer::CreateSequenceDescriptions(CorpusDescriptorPtr corpus, std::string mapPath, size_t labelDimension, bool isMultiCrop)
{
    std::ifstream mapFile(mapPath);
    if (!mapFile)
    {
        RuntimeError("Could not open %s for reading.", mapPath.c_str());
    }

    size_t itemsPerLine = isMultiCrop ? 10 : 1;
    size_t curId = 0;
    std::string line;
    PathReaderMap knownReaders;
    ReaderSequenceMap readerSequences;
    ImageSequenceDescription description;
    description.m_numberOfSamples = 1;

    Timer timer;
    timer.Start();

    auto& stringRegistry = corpus->GetStringRegistry();
    ChunkIdType currentChunkId = 0;
    ImageChunkDescriptionPtr currentChunk = std::make_shared<ImageChunkDescription>();
    for (size_t lineIndex = 0; std::getline(mapFile, line); ++lineIndex)
    {
        std::stringstream ss(line);
        std::string imagePath, classId, sequenceKey;
        // Try to parse sequence id, file path and label.
        if (!std::getline(ss, sequenceKey, '\t') || !std::getline(ss, imagePath, '\t') || !std::getline(ss, classId, '\t'))
        {
            // In case when the sequence key is not specified we set it to the line number inside the mapping file.
            // Assume that only image path and class label is given (old format).
            classId = imagePath;
            imagePath = sequenceKey;
            sequenceKey = std::to_string(lineIndex);

            if (classId.empty() || imagePath.empty())
                RuntimeError("Invalid map file format, must contain 2 or 3 tab-delimited columns, line %" PRIu64 " in file %s.", lineIndex, mapPath.c_str());
        }

        // Skipping sequences that are not included in corpus.
        if (!corpus->IsIncluded(sequenceKey))
        {
            continue;
        }

        char* eptr;
        errno = 0;
        size_t cid = strtoull(classId.c_str(), &eptr, 10);
        if (classId.c_str() == eptr || errno == ERANGE)
            RuntimeError("Cannot parse label value on line %" PRIu64 ", second column, in file %s.", lineIndex, mapPath.c_str());

        if (cid >= labelDimension)
        {
            RuntimeError(
                "Image '%s' has invalid class id '%" PRIu64 "'. Expected label dimension is '%" PRIu64 "'. Line %" PRIu64 " in file %s.",
                imagePath.c_str(), cid, labelDimension, lineIndex, mapPath.c_str());
        }

        if (CHUNKID_MAX < curId + itemsPerLine)
        {
            RuntimeError("Maximum number of chunks exceeded.");
        }

        // Creating new chunk if necessary.
        if (currentChunk->m_numberOfSamples > 511)
        {
            m_chunks.push_back(currentChunk);
            currentChunk = std::make_shared<ImageChunkDescription>();

            currentChunk->m_id = ++currentChunkId;
            currentChunk->m_numberOfSamples = currentChunk->m_numberOfSequences = 0;
            currentChunk->m_startIndex = m_imageSequences.size();
        }

        for (size_t start = curId; curId < start + itemsPerLine; curId++)
        {
            description.m_id = curId;
            description.m_chunkId = (ChunkIdType)currentChunkId;
            description.m_path = imagePath;
            description.m_classId = cid;
            description.m_key.m_sequence = stringRegistry[sequenceKey];
            description.m_key.m_sample = 0;

            m_keyToSequence[description.m_key.m_sequence] = m_imageSequences.size();
            m_imageSequences.push_back(description);
            currentChunk->m_numberOfSamples++;
            currentChunk->m_numberOfSequences++;
            RegisterByteReader(description.m_id, description.m_path, knownReaders, readerSequences);
        }
    }

    // Adding the last chunk.
    if (currentChunk->m_numberOfSamples > 0)
    {
        m_chunks.push_back(currentChunk);
    }

    for (auto& reader : knownReaders)
    {
        reader.second->Register(readerSequences[reader.first]);
    }

    timer.Stop();
    if (m_verbosity > 1)
    {
        fprintf(stderr, "ImageDeserializer: Read information about %d images in %.6g seconds\n", (int)m_imageSequences.size(), timer.ElapsedSeconds());
    }
}

ChunkPtr ImageDataDeserializer::GetChunk(ChunkIdType chunkId)
{
    auto sequenceDescription = m_chunks[chunkId];
    return std::make_shared<ImageChunk>(chunkId, *this);
}

void ImageDataDeserializer::RegisterByteReader(size_t seqId, const std::string& path, PathReaderMap& knownReaders, ReaderSequenceMap& readerSequences)
{
    assert(!path.empty());

    auto atPos = path.find_first_of('@');
    // Is it container or plain image file?
    if (atPos == std::string::npos)
        return;
    // REVIEW alexeyk: only .zip container support for now.
#ifdef USE_ZIP
    assert(atPos > 0);
    assert(atPos + 1 < path.length());
    auto containerPath = path.substr(0, atPos);
    // skip @ symbol and path separator (/ or \)
    auto itemPath = path.substr(atPos + 2);
    // zlib only supports / as path separator.
    std::replace(begin(itemPath), end(itemPath), '\\', '/');
    std::shared_ptr<ByteReader> reader;
    auto r = knownReaders.find(containerPath);
    if (r == knownReaders.end())
    {
        reader = std::make_shared<ZipByteReader>(containerPath);
        knownReaders[containerPath] = reader;
        readerSequences[containerPath] = std::map<std::string, size_t>();
    }
    else
    {
        reader = (*r).second;
    }

    readerSequences[containerPath][itemPath] = seqId;
    m_readers[seqId] = reader;
#else
    UNUSED(seqId);
    UNUSED(knownReaders);
    UNUSED(readerSequences);
    RuntimeError("The code is built without zip container support. Only plain image files are supported.");
#endif
}

ImageDataPtr ImageDataDeserializer::ReadImage(size_t seqId, const std::string& path)
{
    assert(!path.empty());

    ImageDataDeserializer::SeqReaderMap::const_iterator r;
    if (m_readers.empty() || (r = m_readers.find(seqId)) == m_readers.end())
        return m_defaultReader.Read(seqId, path);
    return (*r).second->Read(seqId, path);
}

struct FileMappedData : ImageData
{
    FileMappedData(const std::string& path) 
        : m_fm(path.c_str(), boost::interprocess::read_only),
        m_region(m_fm, boost::interprocess::read_only)
    {
        m_data = (unsigned char*)m_region.get_address();
        m_size = m_region.get_size() / sizeof(unsigned char);
    }

    boost::interprocess::file_mapping m_fm;
    boost::interprocess::mapped_region m_region;
};

ImageDataPtr FileByteReader::Read(size_t, const std::string& path)
{
    return std::make_shared<FileMappedData>(path.c_str());
}

bool ImageDataDeserializer::GetSequenceDescriptionByKey(const KeyType& key, SequenceDescription& result)
{
    auto index = m_keyToSequence.find(key.m_sequence);
    // Checks whether it is a known sequence for us.
    if (key.m_sample != 0 || index == m_keyToSequence.end())
    {
        return false;
    }

    result = m_imageSequences[index->second];
    return true;
}

}}}
