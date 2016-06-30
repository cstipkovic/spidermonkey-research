/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {classes: Cc, interfaces: Ci, utils: Cu, results: Cr} = Components;

Cu.import("resource://gre/modules/Log.jsm");

Cu.import("chrome://marionette/content/atom.js");
Cu.import("chrome://marionette/content/error.js");

const logger = Log.repository.getLogger("Marionette");

/**
 * This module provides shared functionality for dealing with DOM-
 * and web elements in Marionette.
 *
 * A web element is an abstraction used to identify an element when it
 * is transported across the protocol, between remote- and local ends.
 *
 * Each element has an associated web element reference (a UUID) that
 * uniquely identifies the the element across all browsing contexts. The
 * web element reference for every element representing the same element
 * is the same.
 *
 * The @code{element.Store} provides a mapping between web element
 * references and DOM elements for each browsing context.  It also provides
 * functionality for looking up and retrieving elements.
 */

this.EXPORTED_SYMBOLS = ["element"];

const DOCUMENT_POSITION_DISCONNECTED = 1;
const XMLNS = "http://www.w3.org/1999/xhtml";

const uuidGen = Cc["@mozilla.org/uuid-generator;1"]
    .getService(Ci.nsIUUIDGenerator);

this.element = {};

element.Key = "element-6066-11e4-a52e-4f735466cecf";
element.LegacyKey = "ELEMENT";

element.Strategy = {
  ClassName: "class name",
  Selector: "css selector",
  ID: "id",
  Name: "name",
  LinkText: "link text",
  PartialLinkText: "partial link text",
  TagName: "tag name",
  XPath: "xpath",
  Anon: "anon",
  AnonAttribute: "anon attribute",
};

/**
 * Stores known/seen elements and their associated web element
 * references.
 *
 * Elements are added by calling |add(el)| or |addAll(elements)|, and
 * may be queried by their web element reference using |get(element)|.
 */
element.Store = class {
  constructor() {
    this.els = {};
    this.timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
  }

  clear() {
    this.els = {};
  }

  /**
   * Make a collection of elements seen.
   *
   * The oder of the returned web element references is guaranteed to
   * match that of the collection passed in.
   *
   * @param {NodeList} els
   *     Sequence of elements to add to set of seen elements.
   *
   * @return {Array.<WebElement>}
   *     List of the web element references associated with each element
   *     from |els|.
   */
  addAll(els) {
    let add = this.add.bind(this);
    return [...els].map(add);
  }

  /**
   * Make an element seen.
   *
   * @param {nsIDOMElement} el
   *    Element to add to set of seen elements.
   *
   * @return {string}
   *     Web element reference associated with element.
   */
  add(el) {
    for (let i in this.els) {
      let foundEl;
      try {
        foundEl = this.els[i].get();
      } catch (e) {}

      if (foundEl) {
        if (new XPCNativeWrapper(foundEl) == new XPCNativeWrapper(el)) {
          return i;
        }

      // cleanup reference to gc'd element
      } else {
        delete this.els[i];
      }
    }

    let id = element.generateUUID();
    this.els[id] = Cu.getWeakReference(el);
    return id;
  }

  /**
   * Determine if the provided web element reference has been seen
   * before/is in the element store.
   *
   * @param {string} uuid
   *     Element's associated web element reference.
   *
   * @return {boolean}
   *     True if element is in the store, false otherwise.
   */
  has(uuid) {
    return Object.keys(this.els).includes(uuid);
  }

  /**
   * Retrieve a DOM element by its unique web element reference/UUID.
   *
   * @param {string} uuid
   *     Web element reference, or UUID.
   * @param {(nsIDOMWindow|ShadowRoot)} container
   * Window and an optional shadow root that contains the element.
   *
   * @returns {nsIDOMElement}
   *     Element associated with reference.
   *
   * @throws {JavaScriptError}
   *     If the provided reference is unknown.
   * @throws {StaleElementReferenceError}
   *     If element has gone stale, indicating it is no longer attached to
   *     the DOM provided in the container.
   */
  get(uuid, container) {
    let el = this.els[uuid];
    if (!el) {
      throw new JavaScriptError(`Element reference not seen before: ${uuid}`);
    }

    try {
      el = el.get();
    } catch (e) {
      el = null;
      delete this.els[id];
    }

    // use XPCNativeWrapper to compare elements (see bug 834266)
    let wrappedFrame = new XPCNativeWrapper(container.frame);
    let wrappedShadowRoot;
    if (container.shadowRoot) {
      wrappedShadowRoot = new XPCNativeWrapper(container.shadowRoot);
    }

    let wrappedEl = new XPCNativeWrapper(el);
    if (!el ||
        !(wrappedEl.ownerDocument == wrappedFrame.document) ||
        element.isDisconnected(wrappedEl, wrappedFrame, wrappedShadowRoot)) {
      throw new StaleElementReferenceError(
          "The element reference is stale. Either the element " +
          "is no longer attached to the DOM or the page has been refreshed.");
    }

    return el;
  }
};

/**
 * Find a single element or a collection of elements starting at the
 * document root or a given node.
 *
 * If |timeout| is above 0, an implicit search technique is used.
 * This will wait for the duration of |timeout| for the element
 * to appear in the DOM.
 *
 * See the |element.Strategy| enum for a full list of supported
 * search strategies that can be passed to |strategy|.
 *
 * Available flags for |opts|:
 *
 *     |all|
 *       If true, a multi-element search selector is used and a sequence
 *       of elements will be returned.  Otherwise a single element.
 *
 *     |timeout|
 *       Duration to wait before timing out the search.  If |all| is
 *       false, a NoSuchElementError is thrown if unable to find
 *       the element within the timeout duration.
 *
 *     |startNode|
 *       Element to use as the root of the search.
 *
 * @param {Object.<string, Window>} container
 *     Window object and an optional shadow root that contains the
 *     root shadow DOM element.
 * @param {string} strategy
 *     Search strategy whereby to locate the element(s).
 * @param {string} selector
 *     Selector search pattern.  The selector must be compatible with
 *     the chosen search |strategy|.
 * @param {Object.<string, ?>} opts
 *     Options.
 *
 * @return {Promise: (nsIDOMElement|Array<nsIDOMElement>)}
 *     Single element or a sequence of elements.
 *
 * @throws InvalidSelectorError
 *     If |strategy| is unknown.
 * @throws InvalidSelectorError
 *     If |selector| is malformed.
 * @throws NoSuchElementError
 *     If a single element is requested, this error will throw if the
 *     element is not found.
 */
element.find = function(container, strategy, selector, opts = {}) {
  opts.all = !!opts.all;
  opts.timeout = opts.timeout || 0;

  let searchFn;
  if (opts.all) {
    searchFn = findElements.bind(this);
  } else {
    searchFn = findElement.bind(this);
  }

  return new Promise((resolve, reject) => {
    let findElements = implicitlyWaitFor(
        () => find_(container, strategy, selector, searchFn, opts),
        opts.timeout);

    findElements.then(foundEls => {
      // the following code ought to be moved into findElement
      // and findElements when bug 1254486 is addressed
      if (!opts.all && (!foundEls || foundEls.length == 0)) {
        let msg;
        switch (strategy) {
          case element.Strategy.AnonAttribute:
            msg = "Unable to locate anonymous element: " + JSON.stringify(selector);
            break;

          default:
            msg = "Unable to locate element: " + selector;
        }

        reject(new NoSuchElementError(msg));
      }

      if (opts.all) {
        resolve(foundEls);
      }
      resolve(foundEls[0]);
    }, reject);
  });
};

function find_(container, strategy, selector, searchFn, opts) {
  let rootNode = container.shadowRoot || container.frame.document;
  let startNode = opts.startNode || rootNode;

  let res;
  try {
    res = searchFn(strategy, selector, rootNode, startNode);
  } catch (e) {
    throw new InvalidSelectorError(
        `Given ${strategy} expression "${selector}" is invalid: ${e}`);
  }

  if (element.isElementCollection(res)) {
    return res;
  } else if (res) {
    return [res];
  }
  return [];
}

/**
 * Find a value by XPATH
 *
 * @param nsIDOMElement root
 *        Document root
 * @param string value
 *        XPATH search string
 * @param nsIDOMElement node
 *        start node
 *
 * @return nsIDOMElement
 *        returns the found element
 */
function findByXPath(root, value, node) {
  return root.evaluate(value, node, null,
          Ci.nsIDOMXPathResult.FIRST_ORDERED_NODE_TYPE, null).singleNodeValue;
}

/**
 * Find values by XPATH
 *
 * @param nsIDOMElement root
 *        Document root
 * @param string value
 *        XPATH search string
 * @param nsIDOMElement node
 *        start node
 *
 * @return object
 *        returns a list of found nsIDOMElements
 */
function findByXPathAll(root, value, node) {
  let values = root.evaluate(value, node, null,
                    Ci.nsIDOMXPathResult.ORDERED_NODE_ITERATOR_TYPE, null);
  let elements = [];
  let element = values.iterateNext();
  while (element) {
    elements.push(element);
    element = values.iterateNext();
  }
  return elements;
}

/**
 * Finds a single element.
 *
 * @param {element.Strategy} using
 *     Selector strategy to use.
 * @param {string} value
 *     Selector expression.
 * @param {DOMElement} rootNode
 *     Document root.
 * @param {DOMElement=} startNode
 *     Optional node from which to start searching.
 *
 * @return {DOMElement}
 *     Found elements.
 *
 * @throws {InvalidSelectorError}
 *     If strategy |using| is not recognised.
 * @throws {Error}
 *     If selector expression |value| is malformed.
 */
function findElement(using, value, rootNode, startNode) {
  switch (using) {
    case element.Strategy.ID:
      if (startNode.getElementById) {
        return startNode.getElementById(value);
      }
      return findByXPath(rootNode, `.//*[@id="${value}"]`, startNode);

    case element.Strategy.Name:
      if (startNode.getElementsByName) {
        return startNode.getElementsByName(value)[0];
      }
      return findByXPath(rootNode, `.//*[@name="${value}"]`, startNode);

    case element.Strategy.ClassName:
      // works for >= Firefox 3
      return  startNode.getElementsByClassName(value)[0];

    case element.Strategy.TagName:
      // works for all elements
      return startNode.getElementsByTagName(value)[0];

    case element.Strategy.XPath:
      return  findByXPath(rootNode, value, startNode);

    // TODO(ato): Rewrite this, it's hairy:
    case element.Strategy.LinkText:
    case element.Strategy.PartialLinkText:
      let el;
      let allLinks = startNode.getElementsByTagName("A");
      for (let i = 0; i < allLinks.length && !el; i++) {
        let text = allLinks[i].text;
        if (using == element.Strategy.PartialLinkText) {
          if (text.indexOf(value) != -1) {
            el = allLinks[i];
          }
        } else if (text == value) {
          el = allLinks[i];
        }
      }
      return el;

    case element.Strategy.Selector:
      try {
        return startNode.querySelector(value);
      } catch (e) {
        throw new InvalidSelectorError(`${e.message}: "${value}"`);
      }

    case element.Strategy.Anon:
      return rootNode.getAnonymousNodes(startNode);

    case element.Strategy.AnonAttribute:
      let attr = Object.keys(value)[0];
      return rootNode.getAnonymousElementByAttribute(startNode, attr, value[attr]);

    default:
      throw new InvalidSelectorError(`No such strategy: ${using}`);
  }
};

/**
 * Find multiple elements.
 *
 * @param {element.Strategy} using
 *     Selector strategy to use.
 * @param {string} value
 *     Selector expression.
 * @param {DOMElement} rootNode
 *     Document root.
 * @param {DOMElement=} startNode
 *     Optional node from which to start searching.
 *
 * @return {DOMElement}
 *     Found elements.
 *
 * @throws {InvalidSelectorError}
 *     If strategy |using| is not recognised.
 * @throws {Error}
 *     If selector expression |value| is malformed.
 */
function findElements(using, value, rootNode, startNode) {
  switch (using) {
    case element.Strategy.ID:
      value = `.//*[@id="${value}"]`;

    // fall through
    case element.Strategy.XPath:
      return findByXPathAll(rootNode, value, startNode);

    case element.Strategy.Name:
      if (startNode.getElementsByName) {
        return startNode.getElementsByName(value);
      }
      return findByXPathAll(rootNode, `.//*[@name="${value}"]`, startNode);

    case element.Strategy.ClassName:
      return startNode.getElementsByClassName(value);

    case element.Strategy.TagName:
      return startNode.getElementsByTagName(value);

    case element.Strategy.LinkText:
    case element.Strategy.PartialLinkText:
      let els = [];
      let allLinks = startNode.getElementsByTagName("A");
      for (let i = 0; i < allLinks.length; i++) {
        let text = allLinks[i].text;
        if (using == element.Strategy.PartialLinkText) {
          if (text.indexOf(value) != -1) {
            els.push(allLinks[i]);
          }
        } else if (text == value) {
          els.push(allLinks[i]);
        }
      }
      return els;

    case element.Strategy.Selector:
      return startNode.querySelectorAll(value);

    case element.Strategy.Anon:
      return rootNode.getAnonymousNodes(startNode);

    case element.Strategy.AnonAttribute:
      let attr = Object.keys(value)[0];
      let el = rootNode.getAnonymousElementByAttribute(startNode, attr, value[attr]);
      if (el) {
        return [el];
      }
      return [];

    default:
      throw new InvalidSelectorError(`No such strategy: ${using}`);
  }
}

/**
 * Runs function off the main thread until its return value is truthy
 * or the provided timeout is reached.  The function is guaranteed to be
 * run at least once, irregardless of the timeout.
 *
 * A truthy return value constitutes a truthful boolean, positive number,
 * object, or non-empty array.
 *
 * The |func| is evaluated every |interval| for as long as its runtime
 * duration does not exceed |interval|.  If the runtime evaluation duration
 * of |func| is greater than |interval|, evaluations of |func| are queued.
 *
 * @param {function(): ?} func
 *     Function to run off the main thread.
 * @param {number} timeout
 *     Desired timeout.  If 0 or less than the runtime evaluation time
 *     of |func|, |func| is guaranteed to run at least once.
 * @param {number=} interval
 *     Duration between each poll of |func| in milliseconds.  Defaults to
 *     100 milliseconds.
 *
 * @return {Promise}
 *     Yields the return value from |func|.  The promise is rejected if
 *     |func| throws.
 */
function implicitlyWaitFor(func, timeout, interval = 100) {
  let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);

  return new Promise((resolve, reject) => {
    let startTime = new Date().getTime();
    let endTime = startTime + timeout;

    let elementSearch = function() {
      let res;
      try {
        res = func();
      } catch (e) {
        reject(e);
      }

      // empty arrays evaluate to true in JS,
      // so we must first ascertan if the result is a collection
      //
      // we also return immediately if timeout is 0,
      // allowing |func| to be evaluated at least once
      let col = element.isElementCollection(res);
      if (((col && res.length > 0 ) || (!col && !!res)) ||
          (startTime == endTime || new Date().getTime() >= endTime)) {
        resolve(res);
      }
    };

    // the repeating slack timer waits |interval|
    // before invoking |elementSearch|
    elementSearch();

    timer.init(elementSearch, interval, Ci.nsITimer.TYPE_REPEATING_SLACK);

  // cancel timer and propagate result
  }).then(res => {
    timer.cancel();
    return res;
  }, err => {
    timer.cancel();
    throw err;
  });
}

element.isElementCollection = function(seq) {
  if (seq === null) {
    return false;
  }

  const arrayLike = {
    "[object Array]": 0,
    "[object HTMLCollection]": 1,
    "[object NodeList]": 2,
  };

  let typ = Object.prototype.toString.call(seq);
  return typ in arrayLike;
};

element.makeWebElement = function(uuid) {
  return {
    [element.Key]: uuid,
    [element.LegacyKey]: uuid,
  };
};

element.generateUUID = function() {
  let uuid = uuidGen.generateUUID().toString();
  return uuid.substring(1, uuid.length - 1);
};

/**
 * Convert any web elements in arbitrary objects to DOM elements by
 * looking them up in the seen element store.
 *
 * @param {?} obj
 *     Arbitrary object containing web elements.
 * @param {element.Store} seenEls
 *     Element store to use for lookup of web element references.
 * @param {Window} win
 *     Window.
 * @param {ShadowRoot} shadowRoot
 *     Shadow root.
 *
 * @return {?}
 *     Same object as provided by |obj| with the web elements replaced
 *     by DOM elements.
 */
element.fromJson = function(
    obj, seenEls, win, shadowRoot = undefined) {
  switch (typeof obj) {
    case "boolean":
    case "number":
    case "string":
      return obj;

    case "object":
      if (obj === null) {
        return obj;
      }

      // arrays
      else if (Array.isArray(obj)) {
        return obj.map(e => element.fromJson(e, seenEls, win, shadowRoot));
      }

      // web elements
      else if (Object.keys(obj).includes(element.Key) ||
          Object.keys(obj).includes(element.LegacyKey)) {
        let uuid = obj[element.Key] || obj[element.LegacyKey];
        let el = seenEls.get(uuid, {frame: win, shadowRoot: shadowRoot});
        if (!el) {
          throw new WebDriverError(`Unknown element: ${uuid}`);
        }
        return el;
      }

      // arbitrary objects
      else {
        let rv = {};
        for (let prop in obj) {
          rv[prop] = element.fromJson(obj[prop], seenEls, win, shadowRoot);
        }
        return rv;
      }
  }
};

/**
 * Convert arbitrary objects to JSON-safe primitives that can be
 * transported over the Marionette protocol.
 *
 * Any DOM elements are converted to web elements by looking them up
 * and/or adding them to the element store provided.
 *
 * @param {?} obj
 *     Object to be marshaled.
 * @param {element.Store} seenEls
 *     Element store to use for lookup of web element references.
 *
 * @return {?}
 *     Same object as provided by |obj| with the elements replaced by
 *     web elements.
 */
element.toJson = function(obj, seenEls) {
  switch (typeof obj) {
    case "undefined":
      return null;

    case "boolean":
    case "number":
    case "string":
      return obj;

    case "object":
      if (obj === null) {
        return obj;
      }

      // NodeList, HTMLCollection
      else if (element.isElementCollection(obj)) {
        return [...obj].map(el => element.toJson(el, seenEls));
      }

      // DOM element
      else if (obj.nodeType == 1) {
        let uuid = seenEls.add(obj);
        return {[element.Key]: uuid, [element.LegacyKey]: uuid};
      }

      // arbitrary objects
      else {
        let rv = {};
        for (let prop in obj) {
          try {
            rv[prop] = element.toJson(obj[prop], seenEls);
          } catch (e if (e.result == Cr.NS_ERROR_NOT_IMPLEMENTED)) {
            logger.debug(`Skipping ${prop}: ${e.message}`);
          }
        }
        return rv;
      }
  }
};

/**
 * Check if the element is detached from the current frame as well as
 * the optional shadow root (when inside a Shadow DOM context).
 *
 * @param {nsIDOMElement} el
 *     Element to be checked.
 * @param nsIDOMWindow frame
 *     Window object that contains the element or the current host
 *     of the shadow root.
 * @param {ShadowRoot=} shadowRoot
 *     An optional shadow root containing an element.
 *
 * @return {boolean}
 *     Flag indicating that the element is disconnected.
 */
element.isDisconnected = function(el, frame, shadowRoot = undefined) {
  // shadow dom
  if (shadowRoot && frame.ShadowRoot) {
    if (el.compareDocumentPosition(shadowRoot) &
        DOCUMENT_POSITION_DISCONNECTED) {
      return true;
    }

    // looking for next possible ShadowRoot ancestor
    let parent = shadowRoot.host;
    while (parent && !(parent instanceof frame.ShadowRoot)) {
      parent = parent.parentNode;
    }
    return element.isDisconnected(shadowRoot.host, frame, parent);

  // outside shadow dom
  } else {
    let docEl = frame.document.documentElement;
    return el.compareDocumentPosition(docEl) &
        DOCUMENT_POSITION_DISCONNECTED;
  }
};

/**
 * This function generates a pair of coordinates relative to the viewport
 * given a target element and coordinates relative to that element's
 * top-left corner.
 *
 * @param {Node} node
 *     Target node.
 * @param {number=} xOffset
 *     Horizontal offset relative to target's top-left corner.
 *     Defaults to the centre of the target's bounding box.
 * @param {number=} yOffset
 *     Vertical offset relative to target's top-left corner.  Defaults to
 *     the centre of the target's bounding box.
 *
 * @return {Object.<string, number>}
 *     X- and Y coordinates.
 *
 * @throws TypeError
 *     If |xOffset| or |yOffset| are not numbers.
 */
element.coordinates = function(
    node, xOffset = undefined, yOffset = undefined) {

  let box = node.getBoundingClientRect();

  if (typeof xOffset == "undefined" || xOffset === null) {
    xOffset = box.width / 2.0;
  }
  if (typeof yOffset == "undefined" || yOffset === null) {
    yOffset = box.height / 2.0;
  }

  if (typeof yOffset != "number" || typeof xOffset != "number") {
    throw new TypeError("Offset must be a number");
  }

  return {
    x: box.left + xOffset,
    y: box.top + yOffset,
  };
};

/**
 * This function returns true if the node is in the viewport.
 *
 * @param {Element} el
 *     Target element.
 * @param {number=} x
 *     Horizontal offset relative to target.  Defaults to the centre of
 *     the target's bounding box.
 * @param {number=} y
 *     Vertical offset relative to target.  Defaults to the centre of
 *     the target's bounding box.
 *
 * @return {boolean}
 *     True if if |el| is in viewport, false otherwise.
 */
element.inViewport = function(el, x = undefined, y = undefined) {
  let win = el.ownerDocument.defaultView;
  let c = element.coordinates(el, x, y);
  let vp = {
    top: win.pageYOffset,
    left: win.pageXOffset,
    bottom: (win.pageYOffset + win.innerHeight),
    right: (win.pageXOffset + win.innerWidth)
  };

  return (vp.left <= c.x + win.pageXOffset &&
      c.x + win.pageXOffset <= vp.right &&
      vp.top <= c.y + win.pageYOffset &&
      c.y + win.pageYOffset <= vp.bottom);
};

/**
 * This function throws the visibility of the element error if the element is
 * not displayed or the given coordinates are not within the viewport.
 *
 * @param {Element} element
 *     Element to check if visible.
 * @param {Window} window
 *     Window object.
 * @param {number=} x
 *     Horizontal offset relative to target.  Defaults to the centre of
 *     the target's bounding box.
 * @param {number=} y
 *     Vertical offset relative to target.  Defaults to the centre of
 *     the target's bounding box.
 *
 * @return {boolean}
 *     True if visible, false otherwise.
 */
element.isVisible = function(el, x = undefined, y = undefined) {
  let win = el.ownerDocument.defaultView;

  // Bug 1094246: webdriver's isShown doesn't work with content xul
  if (!element.isXULElement(el) && !atom.isElementDisplayed(el, win)) {
    return false;
  }

  if (el.tagName.toLowerCase() == "body") {
    return true;
  }

  if (!element.inViewport(el, x, y)) {
    if (el.scrollIntoView) {
      el.scrollIntoView({block: "start", inline: "nearest"});
      if (!element.inViewport(el)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
};

element.isInteractable = function(el) {
  return element.isPointerInteractable(el) ||
      element.isKeyboardInteractable(el);
};

/**
 * A pointer-interactable element is defined to be the first
 * non-transparent element, defined by the paint order found at the centre
 * point of its rectangle that is inside the viewport, excluding the size
 * of any rendered scrollbars.
 *
 * @param {DOMElement} el
 *     Element determine if is pointer-interactable.
 *
 * @return {boolean}
 *     True if interactable, false otherwise.
 */
element.isPointerInteractable = function(el) {
  let tree = element.getInteractableElementTree(el);
  return tree.length > 0;
};

/**
 * Produces a pointer-interactable elements tree from a given element.
 *
 * The tree is defined by the paint order found at the centre point of
 * the element's rectangle that is inside the viewport, excluding the size
 * of any rendered scrollbars.
 *
 * @param {DOMElement} el
 *     Element to determine if is pointer-interactable.
 *
 * @return {Array.<DOMElement>}
 *     Sequence of non-opaque elements in paint order.
 */
element.getInteractableElementTree = function(el) {
  let doc = el.ownerDocument;
  let win = doc.defaultView;

  // step 1
  // TODO

  // steps 2-3
  let box = el.getBoundingClientRect();
  let visible = {
    width: Math.max(box.x, box.x + box.width) - win.innerWidth,
    height: Math.max(box.y, box.y + box.height) - win.innerHeight,
  };

  // steps 4-5
  let offset = {
    vertical: visible.width / 2.0,
    horizontal: visible.height / 2.0,
  };

  // step 6
  let centre = {
    x: box.x + offset.horizontal,
    y: box.y + offset.vertical,
  };

  // step 7
  let tree = doc.elementsFromPoint(centre.x, centre.y);

  // filter out non-interactable elements
  let rv = [];
  for (let el of tree) {
    if (win.getComputedStyle(el).opacity === "1") {
      rv.push(el);
    }
  }

  return rv;
};

// TODO(ato): Not implemented.
// In fact, it's not defined in the spec.
element.isKeyboardInteractable = function(el) {
  return true;
};

element.isXULElement = function(el) {
  let ns = atom.getElementAttribute(el, "namespaceURI");
  return ns.indexOf("there.is.only.xul") >= 0;
};

const boolEls = {
  audio: ["autoplay", "controls", "loop", "muted"],
  button: ["autofocus", "disabled", "formnovalidate"],
  details: ["open"],
  dialog: ["open"],
  fieldset: ["disabled"],
  form: ["novalidate"],
  iframe: ["allowfullscreen"],
  img: ["ismap"],
  input: ["autofocus", "checked", "disabled", "formnovalidate", "multiple", "readonly", "required"],
  keygen: ["autofocus", "disabled"],
  menuitem: ["checked", "default", "disabled"],
  object: ["typemustmatch"],
  ol: ["reversed"],
  optgroup: ["disabled"],
  option: ["disabled", "selected"],
  script: ["async", "defer"],
  select: ["autofocus", "disabled", "multiple", "required"],
  textarea: ["autofocus", "disabled", "readonly", "required"],
  track: ["default"],
  video: ["autoplay", "controls", "loop", "muted"],
};

/**
 * Tests if the attribute is a boolean attribute on element.
 *
 * @param {DOMElement} el
 *     Element to test if |attr| is a boolean attribute on.
 * @param {string} attr
 *     Attribute to test is a boolean attribute.
 *
 * @return {boolean}
 *     True if the attribute is boolean, false otherwise.
 */
element.isBooleanAttribute = function(el, attr) {
  if (el.namespaceURI !== XMLNS) {
    return false;
  }

  // global boolean attributes that apply to all HTML elements,
  // except for custom elements
  if ((attr == "hidden" || attr == "itemscope") && !el.localName.includes("-")) {
    return true;
  }

  if (!boolEls.hasOwnProperty(el.localName)) {
    return false;
  }
  return boolEls[el.localName].includes(attr)
};
