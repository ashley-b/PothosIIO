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

#include <json.hpp>
using json = nlohmann::json;

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
 * |default ""
 *
 * |param channelIds[Channel IDs] The IDs of channels to enable.
 * If no IDs are specified, all channels will be enabled.
 * |preview disable
 * |default []
 *
 * |param enablePorts[Enable Ports] If true and compatible channels are
 * enabled, enable input ports. This option reserves the IIO buffer for this
 * device, and so can only be enabled for one IIO block per device.
 * |preview disable
 * |default True
 * |widget DropDown()
 * |option [True] True
 * |option [False] False
 *
 * |param bufferSize[Buffer Size] The number of samples to obtain from the IIO
 * device during each refill operation. Larger numbers may reduce overhead but
 * increase latency.
 * |preview disable
 * |default 2048
 * 
 * |factory /iio/source(deviceId, channelIds, enablePorts, bufferSize)
 **********************************************************************/
class IIOSource : public Pothos::Block
{
private:
    std::unique_ptr<IIODevice> dev;
    std::unique_ptr<IIOBuffer> buf;
    std::vector<IIOChannel> channels;
    bool enablePorts;
    size_t bufferSize;
public:
    IIOSource(const std::string &deviceId, const std::vector<std::string> &channelIds,
        const bool &enablePorts, const size_t &bufferSize)
        : enablePorts(enablePorts), bufferSize(bufferSize)
    {
        //expose overlay hook
        this->registerCall(this, POTHOS_FCN_TUPLE(IIOSource, overlay));

        //get libiio context
        IIOContext& ctx = IIOContext::get();

        //if deviceId is blank, create a partial object that exposes the
        //overlay hook for the gui but cannot be activated
        if (deviceId == "") {
            return;
        }

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
            if (c.isScanElement() && this->enablePorts)
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

    std::string overlay(void) const
    {
        IIOContext& ctx = IIOContext::get();

        json topObj;
        auto &params = topObj["params"];

        //configure deviceId dropdown options
        json deviceIdParam;
        deviceIdParam["key"] = "deviceId";
        auto &deviceIdOpts = deviceIdParam["options"];
        deviceIdParam["widgetKwargs"]["editable"] = false;
        deviceIdParam["widgetType"] = "DropDown";

        //add empty device option associated
        json emptyOption;
        emptyOption["name"] = "";
        emptyOption["value"] = "\"\"";
        deviceIdOpts.push_back(emptyOption);

        //enumerate iio devices
        for (auto d : ctx.devices())
        {
            //use the standard label convention, but fall-back on driver/serial
            std::string name;

            json option;
            option["name"] = d.name() + " (" + d.id() + ")";
            option["value"] = "\"" + d.id() + "\"";
            deviceIdOpts.push_back(option);
        }
        params.push_back(deviceIdParam);

        return topObj.dump();
    }

    static Block *make(const std::string &deviceId, const std::vector<std::string> &channelIds,
        const bool &enablePorts, const size_t &bufferSize)
    {
        return new IIOSource(deviceId, channelIds, enablePorts, bufferSize);
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
        if (!this->dev)
        {
            throw Pothos::SystemException("IIOSource::activate()", "no device specified");
        }

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
        if (haveScanElements && this->enablePorts) {
            this->buf = std::unique_ptr<IIOBuffer>(new IIOBuffer(std::move(this->dev->createBuffer(this->bufferSize, false))));
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
            //verify we have enough space in our output buffers to refill
            if (this->workInfo().minOutElements < this->bufferSize)
                return;

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
                    auto outputBuffer = outputPort->buffer();

                    c.read(*this->buf, outputBuffer.as<void*>(), sample_count);
                    outputPort->produce(sample_count);
                }
            }
        }
    }
};

static Pothos::BlockRegistry registerIIOSource(
    "/iio/source", &IIOSource::make);
