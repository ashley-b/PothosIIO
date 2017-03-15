// Copyright (c) 2016 Fiach Antaw
// SPDX-License-Identifier: BSL-1.0

#include <Poco/Error.h>
#include <poll.h>
#include <algorithm>
#include <memory>
#include <string>
#include <cstring>
#include <vector>
#include "IIOSupport.hpp"

/***********************************************************************
 * |PothosDoc IIO Source
 *
 * The IIO source forwards an IIO input device to an output sample stream.
 *
 * |category /IIO
 * |category /Sources
 * |keywords iio industrial io adc sdr
 *
 * |param deviceId[Device ID] The ID of an IIO device on the system.
 * |widget StringEntry()
 * |default ""
 *
 * |param channelIds[Channel IDs] The IDs of channels to enable.
 * If no IDs are specified, all channels will be enabled.
 * |preview disable
 * |default []
 *
 * |factory /iio/source(deviceId, channelIds)
 **********************************************************************/
class IIOSource : public Pothos::Block
{
private:
    std::unique_ptr<IIODevice> dev;
    std::unique_ptr<IIOBuffer> buf;
    std::vector<IIOChannel> channels;
public:
    IIOSource(const std::string &deviceId, const std::vector<std::string> &channelIds)
    {
        //get libiio context
        IIOContext& ctx = IIOContext::get();
        
        //find iio device
        for (auto d : ctx.devices())
        {
            if (d.id() == deviceId)
            {
                this->dev = std::unique_ptr<IIODevice>(new IIODevice(d));
                break;
            }
        }
        if (!this->dev)
        {
            throw Pothos::SystemException("IIOSource::IIOSource()", "device not found");
        }

        //set up probes/setters for device attributes 
        for (auto a : this->dev->attributes())
        {
            Pothos::Callable attrGetter(&IIOSource::getDeviceAttribute);
            Pothos::Callable attrSetter(&IIOSource::setDeviceAttribute);
            attrGetter.bind(std::ref(*this), 0);
            attrGetter.bind(a, 1);
            attrSetter.bind(std::ref(*this), 0);
            attrSetter.bind(a, 1);

            std::string getDeviceAttrName = "deviceAttribute[" + a.name() + "]";
            std::string setDeviceAttrName = "setdeviceAttribute[" + a.name() + "]";
            this->registerCallable(getDeviceAttrName, attrGetter);
            this->registerCallable(setDeviceAttrName, attrSetter);
            this->registerProbe(getDeviceAttrName);
        }

        //set up probes/ports for selected input channels
        for (auto c : this->dev->channels())
        {
            if (c.isOutput())
                continue;
            std::string cId = c.id();
            if (channelIds.size() > 0 && std::none_of(channelIds.begin(), channelIds.end(),
                    [cId](std::string s){ return s == cId; }))
                continue;
            this->channels.push_back(c);

            //set up output ports for scannable input channels
            if (c.isScanElement())
            {
                this->setupOutput(c.id(), c.dtype());
            }

            //set up probes/setters for channel attributes
            for (auto a : c.attributes())
            {
                Pothos::Callable attrGetter(&IIOSource::getChannelAttribute);
                Pothos::Callable attrSetter(&IIOSource::setChannelAttribute);
                attrGetter.bind(std::ref(*this), 0);
                attrGetter.bind(a, 1);
                attrSetter.bind(std::ref(*this), 0);
                attrSetter.bind(a, 1);

                std::string getChannelAttrName = "channelAttribute[" + c.id() + "][" + a.name() + "]";
                std::string setChannelAttrName = "setChannelAttribute[" + c.id() + "][" + a.name() + "]";
                this->registerCallable(getChannelAttrName, attrGetter);
                this->registerCallable(setChannelAttrName, attrSetter);
                this->registerProbe(getChannelAttrName);
            }
        }
    }

    static Block *make(const std::string &deviceId, const std::vector<std::string> &channelIds)
    {
        return new IIOSource(deviceId, channelIds);
    }

    std::string getDeviceAttribute(IIOAttr<IIODevice> a)
    {
        return a.value();
    }

    void setDeviceAttribute(IIOAttr<IIODevice> a, Pothos::Object value)
    {
        a = value.toString();
    }

    std::string getChannelAttribute(IIOAttr<IIOChannel> a)
    {
        return a.value();
    }

    void setChannelAttribute(IIOAttr<IIOChannel> a, Pothos::Object value)
    {
        a = value.toString();
    }

    void activate(void)
    {
        bool haveScanElements = false;
        if (this->buf) {
            this->buf.reset();
        }

        for (auto c : this->channels)
        {
            c.enable();

            if (c.isScanElement())
            {
                haveScanElements = true;
            }
        }

        //create sample buffer if we've got any scan elements
        //buffer size defaults to 4096 samples per buffer, for now
        if (haveScanElements) {
            this->buf = std::unique_ptr<IIOBuffer>(new IIOBuffer(std::move(this->dev->createBuffer(4096, false))));
            if (!this->buf)
            {
                throw Pothos::SystemException("IIOSource::activate()", "buffer creation failed");
            }
            this->buf->setBlockingMode(false);
        }
    }

    void deactivate(void)
    {
        if (this->buf) {
            this->buf.reset();
        }
    }

    void work(void)
    {
        if (this->buf) {
            //wait for samples
            struct pollfd pfd = {
                .fd = this->buf->fd(),
                .events = POLLIN,
                .revents = 0
            };
            struct timespec ts = {
                .tv_sec = static_cast<time_t>(this->workInfo().maxTimeoutNs/10000000),
                .tv_nsec = static_cast<long int>(this->workInfo().maxTimeoutNs % 10000000)
            };
            int ret = ppoll(&pfd, 1, &ts, NULL);
            if (ret < 0)
                throw Pothos::SystemException("IIOSource::work()", "ppoll failed: " + Poco::Error::getMessage(-ret));
            else if (ret == 0)
                return this->yield();

            //get new samples from iio device
            auto bytes_read = this->buf->refill();
            //libiio read operations shouldn't return partial scans
            assert(bytes_read % this->buf->step() == 0);
            auto sample_count = bytes_read / this->buf->step();

            //generate samples
            for (auto c : this->channels)
            {
                if (c.isScanElement()) {
                    auto outputPort = this->output(c.id());
                    auto outputBuffer = outputPort->getBuffer(sample_count);

                    outputBuffer.length = c.read(*this->buf, (void*)outputBuffer.address, sample_count);
                    outputPort->postBuffer(outputBuffer);
                }
            }
        }
    }
};

static Pothos::BlockRegistry registerIIOSource(
    "/iio/source", &IIOSource::make);
