Module["huxeruiCreateBluetooth"] = function(backend) {
  const devices = new Map();
  const links = new Map();
  let nextAttribute = 1;
  let disposed = false;

  function event(name, link, attribute = 0, value = null) {
    if (!disposed) Module["huxeruiBluetoothEvent"](backend, name, link, attribute, value);
  }

  function deviceInfo(device) {
    devices.set(device.id, device);
    return { id: device.id, name: device.name ?? null };
  }

  function bytes(value) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }

  function detachAttributes(link) {
    for (const item of link.attributes.values()) {
      if (item.listener) item.object.removeEventListener("characteristicvaluechanged", item.listener);
    }
    link.attributes.clear();
  }

  function disconnect(id) {
    const link = links.get(id);
    if (!link) return;
    links.delete(id);
    detachAttributes(link);
    link.device.removeEventListener("gattserverdisconnected", link.disconnected);
    link.device.removeEventListener("servicechanged", link.changed);
    link.device.removeEventListener("serviceremoved", link.changed);
    link.device.gatt?.disconnect();
  }

  async function connect(args) {
    const device = devices.get(args.device);
    if (!device?.gatt) throw new DOMException("Bluetooth device is unavailable", "NotFoundError");
    const link = { device, attributes: new Map() };
    link.disconnected = () => {
      if (links.get(args.link) !== link) return;
      disconnect(args.link);
      event("disconnect", args.link);
    };
    link.changed = () => event("services", args.link);
    links.set(args.link, link);
    device.addEventListener("gattserverdisconnected", link.disconnected);
    device.addEventListener("servicechanged", link.changed);
    device.addEventListener("serviceremoved", link.changed);
    try {
      await device.gatt.connect();
      if (links.get(args.link) !== link) {
        if (![...links.values()].some(current => current.device === device)) device.gatt.disconnect();
        throw new DOMException("GATT session was closed", "AbortError");
      }
    } catch (error) {
      if (links.get(args.link) === link) disconnect(args.link);
      throw error;
    }
    return null;
  }

  async function discover(link) {
    const services = await link.device.gatt.getPrimaryServices();
    const attributes = new Map();
    const output = [];
    for (const service of services) {
      const characteristics = [];
      for (const object of await service.getCharacteristics()) {
        const id = nextAttribute++;
        attributes.set(id, { object });
        let descriptors = null;
        if (typeof object.getDescriptors === "function") {
          try {
            descriptors = [];
            for (const descriptor of await object.getDescriptors()) {
              const id = nextAttribute++;
              attributes.set(id, { object: descriptor });
              descriptors.push({ id, uuid: descriptor.uuid });
            }
          } catch (error) {
            if (error.name !== "NotFoundError" && error.name !== "SecurityError" &&
                error.name !== "NotSupportedError") throw error;
            if (error.name !== "NotFoundError") descriptors = null;
          }
        }
        const props = object.properties;
        const properties = {
          read: props.read, write: props.write, writeWithoutResponse: props.writeWithoutResponse,
          notify: props.notify, indicate: props.indicate
        };
        characteristics.push({ id, uuid: object.uuid, properties, descriptors });
      }
      output.push({ uuid: service.uuid, characteristics });
    }
    for (const item of link.attributes.values()) {
      if (item.listener) await item.object.stopNotifications();
    }
    detachAttributes(link);
    link.attributes = attributes;
    return output;
  }

  async function invoke(method, args) {
    if (method === "select") {
      const options = { optionalServices: args.optionalServices };
      if (args.services.length) options.filters = args.services.map(service => ({ services: [service] }));
      else options.acceptAllDevices = true;
      return deviceInfo(await navigator.bluetooth.requestDevice(options));
    }
    if (method === "restore") {
      const device = (await navigator.bluetooth.getDevices()).find(device => device.id === args.device);
      if (!device) throw new DOMException("The browser no longer grants access to the device", "NotFoundError");
      return deviceInfo(device);
    }
    if (method === "connect") return connect(args);
    const link = links.get(args.link);
    if (!link) throw new DOMException("GATT session was closed", "NetworkError");
    if (method === "discover") return discover(link);

    const item = link.attributes.get(args.attribute);
    if (!item) throw new DOMException("GATT attribute is stale", "InvalidStateError");
    const object = item.object;
    if (method === "read") return bytes(await object.readValue());
    if (method === "write") {
      if (args.descriptor) await object.writeValue(args.bytes);
      else if (args.withResponse) {
        if (object.writeValueWithResponse) await object.writeValueWithResponse(args.bytes);
        else throw new DOMException("This browser cannot select the write mode", "NotSupportedError");
      } else {
        if (object.writeValueWithoutResponse) await object.writeValueWithoutResponse(args.bytes);
        else throw new DOMException("This browser cannot select the write mode", "NotSupportedError");
      }
    } else if (method === "notify") {
      if (args.enabled) {
        const listener = value => event("value", args.link, args.attribute, bytes(value.target.value));
        object.addEventListener("characteristicvaluechanged", listener);
        try {
          await object.startNotifications();
          item.listener = listener;
          if (links.get(args.link) !== link) object.removeEventListener("characteristicvaluechanged", listener);
        } catch (error) {
          object.removeEventListener("characteristicvaluechanged", listener);
          throw error;
        }
      } else if (item.listener) {
        await object.stopNotifications();
        object.removeEventListener("characteristicvaluechanged", item.listener);
        item.listener = undefined;
      }
    }
    return null;
  }

  return {
    invoke(ticket, method, args) {
      invoke(method, args).then(value => {
        if (!disposed) Module["huxeruiBluetoothReply"](backend, ticket, true, value);
      }, error => {
        if (!disposed) Module["huxeruiBluetoothReply"](backend, ticket, false, {
          name: String(error?.name ?? "Error"), message: String(error?.message ?? error)
        });
      });
    },
    disconnect,
    dispose() {
      disposed = true;
      for (const id of [...links.keys()]) disconnect(id);
      devices.clear();
    }
  };
};
