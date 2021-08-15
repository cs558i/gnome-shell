/* exported addObjectSignalMethods */
const { GObject } = imports.gi;

class SignalManager {
    /**
     * @returns {SignalManager} - the SignalManager singleton
     */
    static getDefault() {
        if (!this._singleton)
            this._singleton = new SignalManager();
        return this._singleton;
    }

    constructor() {
        this._signalTrackers = new Map();
    }

    /**
     * @param {Object} obj - object to get signal tracker for
     * @returns {SignalTracker} - the signal tracker for object
     */
    getSignalTracker(obj) {
        if (!this._signalTrackers.has(obj))
            this._signalTrackers.set(obj, new SignalTracker(obj));
        return this._signalTrackers.get(obj);
    }
}

class SignalTracker {
    /**
     * @param {Object=} owner - object that owns the tracker
     */
    constructor(owner) {
        if (this._hasDestroySignal(owner))
            this._ownerDestroyId = owner.connect('destroy', () => this.clear());

        this._owner = owner;
        this._map = new Map();
    }

    /**
     * @private
     * @param {Object} obj - an object
     * @returns {bool} - true if obj has a 'destroy' GObject signal
     */
    _hasDestroySignal(obj) {
        return obj instanceof GObject.Object &&
            GObject.signal_lookup('destroy', obj);
    }

    /**
     * @typedef SignalData
     * @property {number[]} ownerSignals - a list of handler IDs
     * @property {number} destroyId - destroy handler ID of tracked object
     */

    /**
     * @private
     * @param {Object} obj - a tracked object
     * @returns {SignalData} - signal data for object
     */
    _getSignalData(obj) {
        if (!this._map.has(obj))
            this._map.set(obj, { ownerSignals: [], destroyId: 0 });
        return this._map.get(obj);
    }

    /**
     * @private
     * @param {GObject.Object} obj - tracked widget
     */
    _trackDestroy(obj) {
        const signalData = this._getSignalData(obj);
        if (signalData.destroyId)
            return;
        signalData.destroyId = obj.connect('destroy', () => this.untrack(obj));
    }

    _disconnectSignal(obj, id) {
        const proto = obj instanceof GObject.Object
            ? GObject.Object.prototype
            : Object.getPrototypeOf(obj);
        proto['disconnect'].call(obj, id);
    }

    /**
     * @param {Object} obj - tracked object
     * @param {...number} handlerIds - tracked handler IDs
     * @returns {void}
     */
    track(obj, ...handlerIds) {
        if (this._hasDestroySignal(obj))
            this._trackDestroy(obj);

        this._getSignalData(obj).ownerSignals.push(...handlerIds);
    }

    /**
     * @param {Object} obj - tracked object instance
     * @returns {void}
     */
    untrack(obj) {
        const { ownerSignals, destroyId } = this._getSignalData(obj);
        this._map.delete(obj);

        ownerSignals.forEach(id => this._disconnectSignal(this._owner, id));
        if (destroyId)
            this._disconnectSignal(obj, destroyId);
    }

    /**
     * @returns {void}
     */
    clear() {
        [...this._map.keys()].forEach(obj => this.untrack(obj));
    }

    /**
     * @returns {void}
     */
    destroy() {
        this.clear();

        if (this._ownerDestroyId)
            this._disconnectSignal(this._owner, this._ownerDestroyId);

        delete this._ownerDestroyId;
        delete this._owner;
    }
}

/**
 * Connect one or more signals, and associate the handlers
 * with a tracked object.
 *
 * All handlers for a particular object can be disconnected
 * by calling disconnectObject(). If object is a {Clutter.widget},
 * this is done automatically when the widget is destroyed.
 *
 * @param {object} thisObj - the emitter object
 * @param {...any} args - a sequence of signal-name/handler pairs
 * with an optional flags value, followed by an object to track
 * @returns {void}
 */
function connectObject(thisObj, ...args) {
    const getParams = argArray => {
        const [signalName, handler, arg, ...rest] = argArray;
        if (typeof arg !== 'number')
            return [signalName, handler, 0, arg, ...rest];

        const flags = arg;
        if (flags > GObject.ConnectFlags.SWAPPED)
            throw new Error(`Invalid flag value ${flags}`);
        if (flags === GObject.ConnectFlags.SWAPPED)
            throw new Error('Swapped signals are not supported');
        return [signalName, handler, flags, ...rest];
    };

    const connectSignal = (emitter, signalName, handler, flags) => {
        const isGObject = emitter instanceof GObject.Object;
        const func = flags === GObject.ConnectFlags.AFTER && isGObject
            ? 'connect_after'
            : 'connect';
        const emitterProto = isGObject
            ? GObject.Object.prototype
            : Object.getPrototypeOf(emitter);
        return emitterProto[func].call(emitter, signalName, handler);
    };

    const signalIds = [];
    while (args.length > 1) {
        const [signalName, handler, flags, ...rest] = getParams(args);
        signalIds.push(connectSignal(thisObj, signalName, handler, flags));
        args = rest;
    }

    let [obj] = args;
    if (!obj)
        obj = globalThis;

    const tracker = SignalManager.getDefault().getSignalTracker(thisObj);
    tracker.track(obj, ...signalIds);
}

/**
 * Disconnect all signals that were connected for
 * the specified tracked object
 *
 * @param {Object} thisObj - the emitter object
 * @param {Object} obj - the tracked object
 * @returns {void}
 */
function disconnectObject(thisObj, obj) {
    SignalManager.getDefault().getSignalTracker(thisObj).untrack(obj);
}

/**
 * Add connectObject()/disconnectObject() methods
 * to prototype. The prototype must have the connect()
 * and disconnect() signal methods.
 *
 * @param {prototype} proto - a prototype
 */
function addObjectSignalMethods(proto) {
    proto['connectObject'] = function (...args) {
        connectObject(this, ...args);
    };
    proto['connect_object'] = proto['connectObject'];

    proto['disconnectObject'] = function (obj) {
        disconnectObject(this, obj);
    };
    proto['disconnect_object'] = proto['disconnectObject'];
}
