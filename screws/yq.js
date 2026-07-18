#!/usr/bin/env bun
// yq/xq/hq — convert XML/HTML/YAML through jq, optionally back to native format
//
// WHAT IT DOES
//   Read XML, HTML, or YAML, convert to JSON, pipe through jq filter,
//   output filtered JSON or convert back to the original format (-X/-H/-Y).
//   Drop-in for kislyuk/xq and kislyuk/yq using bun native modules.
//
//   Multi-name: invoked as yq → defaults to YAML in/out;
//   xq → XML/HTML, JSON out; hq → HTML in/out.
//
// USAGE
//   xq [options] <jq_filter> [file...]
//
// OPTIONS
//   Input format (default: auto-detect by content or extension):
//     -x, --xml             Force XML input
//     -h, --html            Force HTML input
//     -y, --yaml            Force YAML input
//   Output format:
//     -J, --json-output     Explicit JSON output
//     -X, --xml-output      Convert JSON output to XML
//     -H, --html-output     Convert JSON output to HTML
//     -Y, --yaml-output     Convert JSON output to YAML
//   XML:  --xml-root NAME, --xml-force-list ELT
//   YAML: --yaml-indent N, --yaml-indentless-lists, --yaml-explicit-start,
//         --yaml-explicit-end, --yaml-width N
//   Other:
//     -i, --in-place        Edit files in place (requires -X, -H, -J, -Y)
//         --help            Show help
//
// EXAMPLES
//   cat config.xml | xq '.slave.label'
//   cat config.xml | xq -X '.slave.label = "new value"'
//   curl -s example.com | xq '.div.content'
//   cat data.yaml | xq -Y '.users[] | {name, email}'
//   xq -y -Y '.key' config.yaml > config.yaml  # YAML roundtrip
//
// EXIT CODES
//   0   Success
//   1   Parse error, IO error, jq error
//   <n> jq exit code forwarded
//
// DEPENDENCIES
//   jq        https://jqlang.github.io/jq/
//   xmldom    https://github.com/xmldom/xmldom
//   htmlparser2  https://github.com/fb55/htmlparser2
//   js-yaml   https://github.com/nodeca/js-yaml
//
// ============================================================================
// TUTORIAL: THE XQ FORMAT
// ============================================================================
//
// XML/HTML elements map to JSON objects.
//
//   <tag attr="val">text</tag>
//
// becomes:
//
//   {"tag":{"@attr":"val","#text":"text"}}
//
// Attributes get @ prefix.  Text content gets #text.
//
// Multiple same-named children become an array:
//
//   <r><i>a</i><i>b</i></r>
//
// becomes:
//
//   {"r":{"i":["a","b"]}}
//
// YAML loads as plain JS — no @attr/#text wrapper.

import { DOMParser } from 'xmldom';
import { parseDocument } from 'htmlparser2';
import serialize from 'dom-serializer';
import * as yaml from 'js-yaml';
import fs from 'fs';
import { spawnSync } from 'child_process';

function invocationName() {
    // env._ has the invocation path (preserves symlink names like xq -> yq.js).
    // argv[1] has the resolved script path — wrong when used via a symlink.
    // Only use env._ when it actually names a script (not the interpreter).
    const shellName = (process.env._ || '').split('/').pop() || '';
    const scriptName = (process.argv[1] || '').split('/').pop() || '';
    const via = (shellName && shellName !== 'bun') ? shellName : scriptName;

    return via.replace(/\.js$/i, '').toLowerCase();
}

const INVOKED_AS = invocationName();
const BIN = INVOKED_AS || 'yq';
const ELEMENT = 1;
const TEXT = 3;
const CDATA = 4;

// Invocation-specific input/output format defaults.
const INVOCATION_MODE = {
    yq: { input: 'yaml', output: 'yaml' },
    xq: { input: 'xml', output: 'xml' },
    hq: { input: 'html', output: 'html' },
}[INVOKED_AS] || {};

// ======== Core predicates ========

function isTextNode(n)      { return n.nodeType === TEXT || n.nodeType === CDATA; }
function isElementNode(n)   { return n.nodeType === ELEMENT; }
function isArray(x)         { return Array.isArray(x); }
function isPlainObject(v)   { return typeof v === 'object' && v !== null && !isArray(v); }
function hasKeys(o)         { return Object.keys(o).length > 0; }
function isPrimitive(v)     { return typeof v === 'string' || typeof v === 'number' || typeof v === 'boolean'; }
function hasChildren(map, key) { return key in map; }
function isWhitespace(s)    { return isString(s) && s.trim() === ''; }
function isFatalLevel(lvl)  { return lvl === 'error' || lvl === 'fatalError'; }
function isNodeList(nodes)  { return typeof nodes.item === 'function'; }
function isAttr(k)          { return k.startsWith('@'); }
function isTextContent(k)   { return k === '#text'; }
function isMixedKey(k)      { return k === '#mixed'; }
function isMixedContent(val) { return isArray(val) && val.some(isPrimitive) && val.some(isPlainObject); }
function isXmlnsDecl(k)     { return k.startsWith('@xmlns'); }
function areAllText(items)  { return items.every(isPrimitive); }
function areAllElements(items) { return items.every(isPlainObject); }
function isEmpty(arr)       { return arr.length === 0; }
function isEmptyString(s)   { return s === ''; }
function isString(s)        { return typeof s === 'string'; }
function hasItems(arr)     { return arr.length > 0; }
function hasFiles(files)    { return hasItems(files); }
function hasForceList(opts) { return hasItems(opts.forceList); }
function jqFailed(proc)     { return proc.status !== 0; }
function hasXmlSpecialChars(s) { return s.includes('&') || s.includes('<') || s.includes('>'); }
function hasCdataEnd(s)     { return s.includes(']]>'); }
function needsNsDecl(nsTag, declared, nsMap) {
    return nsTag && !declared.has(nsTag.uri) && !(nsTag.uri in nsMap);
}

function isSingleFile(files)    { return files.length === 1; }
function isEmptyMap(map)        { return Object.keys(map).length === 0; }
function isLongFlag(arg)        { return arg.startsWith('--'); }
function isShortFlag(arg)       { return arg.startsWith('-') && !arg.startsWith('--') && arg.length > 1; }
function isSelfClosing(children, text) { return isEmpty(children) && !text; }
function isSingularElement(children, tag) { return hasChildren(children, tag) && !isArray(children[tag]); }

/** @param {object} attrs
 *  @param {string} key
 *  @param {*} val */
function wrapWithAttrs(attrs, key, val) {
    return hasKeys(attrs) ? { ...attrs, [key]: val } : val;
}

function isPendingNamespace(nsPending, declaredUris, nsTag) {
    return nsTag && nsPending && !declaredUris.has(nsTag.uri);
}

// ======== Shared utilities ========

function indentPad(n)  { return '  '.repeat(n); }

function flattenText(v) { return isArray(v) ? v.join('') : String(v); }

// ======== Namespace helpers ========

/**
 * Parse a key in {uri}local format.
 * @param {string} k
 * @returns {{uri:string, local:string}|null}
 */
function parseKey(k) {
    const m = k.match(/^\{([^}]+)\}(.+)$/);
    return m ? { uri: m[1], local: m[2] } : null;
}

/**
 * Get the namespaced key for a DOM element node.
 * Returns {uri}local when a namespace is present, tagName otherwise.
 * @param {object} node - DOM element node
 * @returns {string}
 */
function elementKey(node) {
    const ns = node.namespaceURI;

    if (!ns) return node.tagName;
    if (node.prefix) return `${node.prefix}:${node.localName}`;
    return node.localName || node.tagName;
}

/**
 * Get the namespaced attribute key.
 * Returns @{uri}local when a namespace is present, @name otherwise.
 * @param {object} a - DOM attribute node
 * @returns {string}
 */
function attrKey(a) {
    const ns = a.namespaceURI;
    const name = a.name || a.localName;

    // xmlns:prefix and xmlns declarations use the XML Namespaces URI
    if (ns === 'http://www.w3.org/2000/xmlns/') {
        const hasPrefix = name.includes(':');
        const local = hasPrefix ? name.split(':').pop() : '';
        return hasPrefix ? `@xmlns:${local}` : '@xmlns';
    }

    const localName = name.split(':').pop();
    return ns ? `@{${ns}}${localName}` : `@${name}`;
}

/**
 * Get or auto-assign a namespace prefix in the local map.
 * Generates ns1, ns2, etc. for URIs without an existing prefix.
 * @param {string} uri
 * @param {object} localNs - mutable namespace map (uri → prefix)
 * @returns {string} prefix (may be '' for default namespace)
 */
function ensureNsPrefix(uri, localNs) {
    if (uri in localNs) return localNs[uri];

    let n = 1;
    const used = new Set(Object.values(localNs));

    while (used.has(`ns${n}`)) n++;
    const prefix = `ns${n}`;
    localNs[uri] = prefix;
    return prefix;
}

// ======== Core infrastructure ========

/** @param {string} msg */
function die(msg) {
    console.error(`${BIN}: error: ${msg}`);
    process.exit(1);
}

/** @param {object} opts
 *  @returns {Set|null} */
function buildForceSet(opts) {
    return hasForceList(opts) ? new Set(opts.forceList) : null;
}

/** @param {string[]} files */
function readInput(files) {
    if (hasFiles(files))
        return files.map(f => fs.readFileSync(f, 'utf-8')).join('');

    return fs.readFileSync('/dev/stdin', 'utf-8');
}

/** @param {string} json
 *  @param {string[]} args */
function runJq(json, args) {
    const proc = spawnSync('jq', args, { input: json });

    if (jqFailed(proc)) {
        process.stderr.write(proc.stderr.toString());
        process.exit(proc.status);
    }

    return proc.stdout.toString().trimEnd();
}

/**
 * @param {string} text
 * @returns {object|null}
 */
function tryParseJson(s) {
    try { return JSON.parse(s); }
    catch { return null; }
}

/** @param {string} buf
 *  @param {object[]} docs
 *  @param {string} line */
function tryCompleteDoc(buf, docs, line) {
    buf += line + '\n';
    const value = tryParseJson(buf.trim());

    if (value !== null) {
        docs.push(value);
        return '';
    }
    return buf;
}

/** @param {string} text
 *  @returns {object[]} */
function parseNdjson(text) {
    const docs = [];
    let buf = '';

    for (const line of text.split('\n'))
        buf = tryCompleteDoc(buf, docs, line);

    return docs;
}

/** @param {string} text
 *  @returns {object[]} */
function parseDocs(text) {
    const trimmed = text.trim();
    if (!trimmed) return [{}];

    const single = tryParseJson(trimmed);
    if (single !== null) return [single];

    const docs = parseNdjson(text);
    if (docs.length === 0) die('jq output not valid JSON');

    return docs;
}

// ======== DOM walker (shared by XML/HTML) ========

/** @param {NodeList|Node[]} nodes
 *  @returns {Node[]} */
function normalizeChildren(nodes) {
    if (!isNodeList(nodes)) return nodes;

    const arr = [];

    for (let i = 0; i < nodes.length; i++)
        arr.push(nodes.item(i));

    return arr;
}

/** @param {object} node
 *  @returns {object} */
function collectAttrs(node) {
    const attrs = {};
    const raw = node.attributes;

    if (!raw || typeof raw.length !== 'number')
        return attrs;

    for (let i = 0; i < raw.length; i++) {
        const a = raw[i];
        if (a) attrs[attrKey(a)] = a.value;
    }

    return attrs;
}

/** @param {object} node
 *  @param {string[]} textFrags */
function collectText(node, textFrags) {
    const v = node.nodeValue || '';
    textFrags.push(v);
}

/** @param {object} node
 *  @param {Set|null} forceSet */
function gatherContent(node, forceSet) {
    const items = [];

    for (const child of normalizeChildren(node.childNodes)) {
        if (!child) continue;

        if (isTextNode(child)) {
            collectText(child, items);
        } else if (isElementNode(child)) {
            items.push({ [elementKey(child)]: walk(child, forceSet) });
        }
    }

    // Strip whitespace-only text nodes (formatting whitespace between elements).
    // Preserve non-whitespace text content; single strings are handled upstream.
    return items.filter(item => !(isString(item) && isWhitespace(item)));
}

/** @param {object} node
 *  @param {Set|null} forceSet */
function walk(node, forceSet) {
    if (isTextNode(node))
        return node.nodeValue || '';
    if (!isElementNode(node))
        return null;

    const items = gatherContent(node, forceSet);
    const attrs = collectAttrs(node);



    if (isEmpty(items))
        return hasKeys(attrs) ? attrs : null;
    if (areAllText(items))
        return wrapWithAttrs(attrs, '#text', items.join(''));
    if (areAllElements(items))
        return buildElementResult(items, attrs, forceSet);

    return wrapWithAttrs(attrs, '#mixed', items);
}

/** @param {object[]} items
 *  @param {Set|null} forceSet */
function mergeChildren(items, forceSet) {
    const children = {};

    for (const item of items) {
        const [[tag, val]] = Object.entries(item);
        pushChild(children, tag, val);
    }

    if (forceSet)
        forceArrayElements(children, forceSet);

    return children;
}

/** @param {object[]} items
 *  @param {object} attrs
 *  @param {Set|null} forceSet */
function buildElementResult(items, attrs, forceSet) {
    const children = mergeChildren(items, forceSet);

    if (hasKeys(attrs))
        return { ...attrs, ...children };
    if (hasKeys(children))
        return children;
    return null;
}

/** @param {object} map
 *  @param {string} key
 *  @param {*} val */
function pushChild(map, key, val) {
    if (!hasChildren(map, key))
        return (map[key] = val);

    if (!isArray(map[key]))
        map[key] = [map[key]];
    map[key].push(val);
}

/** @param {object} children
 *  @param {Set|null} forceSet */
function forceArrayElements(children, forceSet) {
    for (const tag of forceSet)
        if (isSingularElement(children, tag))
            children[tag] = [children[tag]];
}

// ======== Format: XML ========

/** @param {string} str
 *  @returns {object} */
function parseDtdEntities(str) {
    const entities = {};

    const doctypeMatch = str.match(/<!DOCTYPE\s+\w+(?:\s+(?:PUBLIC|SYSTEM)\s+"[^"]*"(?:\s+"[^"]*")?)?\s*\[([^\]]*)\]\s*>/i);
    if (!doctypeMatch) return entities;

    const entityRe = /<!ENTITY\s+(\w+)\s+"([^"]*)"\s*>|<!ENTITY\s+(\w+)\s+'([^']*)'\s*>/g;
    let match;

    while ((match = entityRe.exec(doctypeMatch[1])) !== null) {
        const name = match[1] || match[3];
        const value = match[2] !== undefined ? match[2] : (match[4] || '');
        entities[name] = resolveDtdValue(value, entities);
    }

    return entities;
}


const HTML_ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };

/** @param {string} value
 *  @param {object} entities */
function resolveDtdValue(value, entities) {
    return value.replace(/&(\w+);/g, (_, ref) => {
        if (ref in entities) return entities[ref];
        if (ref in HTML_ENTITIES) return HTML_ENTITIES[ref];
        return `&${ref};`;
    });
}


/** @param {string} str
 *  @param {object} entities */
function replaceDtdRefs(str, entities) {
    if (isEmptyMap(entities)) return str;

    const names = Object.keys(entities).sort((a, b) => b.length - a.length);
    const escNames = names.map(n => n.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
    const pattern = new RegExp(`&(${escNames.join('|')});`, 'g');

    return str.replace(pattern, (_, name) => entities[name]);
}



/** @param {string} str
 *  @returns {object} */
function parseXmlString(str) {
    let parseErr = null;

    const parser = new DOMParser({
        errorHandler: lvl => {
            if (isFatalLevel(lvl))
                parseErr = 'parse';
        },
    });

    const doc = parser.parseFromString(str, 'text/xml');

    if (parseErr) die('parse error');
    if (!doc.documentElement) die('empty document');

    return doc.documentElement;
}

/** @param {string} str
 *  @param {object} opts */
function parseXml(str, opts) {
    const resolved = replaceDtdRefs(str, parseDtdEntities(str));
    const root = parseXmlString(resolved);

    return { rootTag: root.tagName, value: walk(root, buildForceSet(opts)) };
}


/** @param {string} s */
function escapeXmlText(s) {
    return s.replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
}

/** @param {string} s */
function escapeOrCdata(s) {
    if (hasCdataEnd(s))
        return escapeXmlText(s);

    if (hasXmlSpecialChars(s))
        return `<![CDATA[${s}]]>`;

    return escapeXmlText(s);
}

/** @param {string|object} item
 *  @param {number} indent
 *  @param {object} nsMap */
function xqRenderMixedItem(item, indent, nsMap) {
    const pad = indentPad(indent);

    if (isString(item))
        return `${pad}  ${escapeOrCdata(item)}\n`;

    return xqToXml(Object.values(item)[0], Object.keys(item)[0], indent + 1, { ...nsMap }) + '\n';
}

/** @param {string} uri
 *  @param {string} prefix
 *  @param {object} localNs
 *  @param {Set} declared
 *  @param {string[]} decls */
function declareNamespace(uri, prefix, localNs, declared, decls) {
    if (!(uri in localNs)) localNs[uri] = prefix;
    declared.add(uri);

    if (prefix)
        decls.push(` xmlns:${prefix}="${escapeXmlText(uri)}"`);
    else
        decls.push(` xmlns="${escapeXmlText(uri)}"`);
}

/** @param {string} k
 *  @param {string} v
 *  @param {object} localNs
 *  @param {Set} declared
 *  @param {string[]} decls */
function collectEntryXmlns(k, v, localNs, declared, decls) {
    if (k === '@xmlns')
        return declareNamespace(String(v), '', localNs, declared, decls);

    if (k.startsWith('@xmlns:'))
        declareNamespace(String(v), k.slice(7), localNs, declared, decls);
}

/** @param {object} value
 *  @param {object} nsMap
 *  @param {{uri:string,local:string}|null} nsTag */
function collectXmlns(value, nsMap, nsTag) {
    const localNs = { ...nsMap };
    const decls = [];
    const declared = new Set();

    for (const [k, v] of Object.entries(value))
        collectEntryXmlns(k, v, localNs, declared, decls);

    if (needsNsDecl(nsTag, declared, nsMap)) {
        const prefix = ensureNsPrefix(nsTag.uri, localNs);
        if (prefix)
            decls.push(` xmlns:${prefix}="${escapeXmlText(nsTag.uri)}"`);
    }

    return { localNs, xmlnsDecls: decls, declaredUris: declared };
}


/** @param {string} tag
 *  @param {object} nsMap */
function resolveXmlTag(tag, nsMap) {
    const nsTag = parseKey(tag);
    if (!nsTag) return tag;

    const prefix = ensureNsPrefix(nsTag.uri, nsMap);
    return prefix ? `${prefix}:${nsTag.local}` : nsTag.local;
}

/** @param {*} value
 *  @param {string} pad
 *  @param {string} tag
 *  @param {string|undefined} decl */
function wrapXmlPrimitive(value, pad, tag, decl) {
    const s = String(value);

    if (isEmptyString(s))
        return `${pad}<${tag}${decl || ''}></${tag}>`;

    return `${pad}<${tag}${decl || ''}>${escapeOrCdata(s)}</${tag}>`;
}

/** @param {*[]} value
 *  @param {string} pad
 *  @param {string} tag
 *  @param {string} attrStr
 *  @param {number} indent
 *  @param {object} nsMap */
function wrapXmlMixedArray(value, pad, tag, attrStr, indent, nsMap) {
    const parts = value.map(item => xqRenderMixedItem(item, indent, nsMap));
    return `${pad}<${tag}${attrStr}>\n${parts.join('')}${pad}</${tag}>`;
}

/** @param {string[]} attrs
 *  @param {string} k
 *  @param {*} v
 *  @param {object} localNs
 *  @param {Set} declaredUris
 *  @param {object} nsMap
 *  @param {string[]} xmlnsDecls */
function appendXmlAttr(attrs, k, v, localNs, declaredUris, nsMap, xmlnsDecls) {
    const name = k.slice(1);
    const nsAttr = parseKey(name);
    const valStr = escapeXmlText(String(v));

    if (!nsAttr) {
        attrs.push(` ${name}="${valStr}"`);
        return;
    }

    const prefix = ensureNsPrefix(nsAttr.uri, localNs);

    if (!declaredUris.has(nsAttr.uri) && !(nsAttr.uri in nsMap)) {
        xmlnsDecls.push(` xmlns:${prefix}="${escapeXmlText(nsAttr.uri)}"`);
        declaredUris.add(nsAttr.uri);
    }

    attrs.push(` ${prefix}:${nsAttr.local}="${valStr}"`);
}


/** @param {object} value
 *  @param {object} localNs
 *  @param {Set} declaredUris
 *  @param {object} nsMap
 *  @param {string[]} xmlnsDecls */
function classifyXmlChildren(value, localNs, declaredUris, nsMap, xmlnsDecls) {
    const attrs = [];
    const children = [];
    const mixedItems = [];
    let text = '';

    for (const [k, v] of Object.entries(value)) {


        if (isXmlnsDecl(k)) continue;

        if (isMixedKey(k)) {
            mixedItems.push(...v);
        } else if (isTextContent(k)) {
            text = flattenText(v);
        } else if (isAttr(k)) {
            appendXmlAttr(attrs, k, v, localNs, declaredUris, nsMap, xmlnsDecls);
        } else if (isArray(v)) {
            children.push(...v.map(item => ({ tag: k, value: item })));
        } else {
            children.push({ tag: k, value: v });
        }
    }

    return { attrs, children, mixedItems, text };
}

/** @param {string} uri
 *  @param {object} nsMap */
function mkNsDeclStr(uri, nsMap) {
    const prefix = nsMap[uri];

    return prefix
        ? ` xmlns:${prefix}="${escapeXmlText(uri)}"`
        : ` xmlns="${escapeXmlText(uri)}"`;
}

/** @param {object} value
 *  @param {string} pad
 *  @param {string} resolvedTag
 *  @param {number} indent
 *  @param {object} nsMap
 *  @param {{uri:string,local:string}|null} nsTag
 *  @param {boolean} nsPending
 *  @param {string|undefined} tagDecl */
function renderXmlObject(value, pad, resolvedTag, indent, nsMap, nsTag, nsPending, tagDecl) {
    const { localNs, xmlnsDecls, declaredUris } = collectXmlns(value, nsMap, nsTag);

    const extraDecls = isPendingNamespace(nsPending, declaredUris, nsTag)
        ? [tagDecl]
        : [];

    const { attrs, children, mixedItems, text } = classifyXmlChildren(
        value, localNs, declaredUris, nsMap,
        [...xmlnsDecls, ...extraDecls]
    );

    const attrStr = buildAttrString(attrs, xmlnsDecls, extraDecls);

    if (!isEmpty(mixedItems))
        return wrapXmlMixedArray(mixedItems, pad, resolvedTag, attrStr, indent, localNs);

    return renderXmlChildren(pad, resolvedTag, attrStr, children, text, indent, localNs);
}

/** @param {string[]} attrs
 *  @param {string[]} xmlnsDecls
 *  @param {string[]} extraDecls */
function buildAttrString(attrs, xmlnsDecls, extraDecls) {
    return attrs.join('') + xmlnsDecls.join('') + extraDecls.join('');
}

/** @param {string} pad
 *  @param {string} tag
 *  @param {string} attrStr
 *  @param {object[]} children
 *  @param {string} text
 *  @param {number} indent
 *  @param {object} localNs */
function renderXmlChildren(pad, tag, attrStr, children, text, indent, localNs) {
    if (isSelfClosing(children, text))
        return `${pad}<${tag}${attrStr}/>`;

    if (isEmpty(children))
        return `${pad}<${tag}${attrStr}>${escapeOrCdata(text)}</${tag}>`;

    const body = children.map(c => xqToXml(c.value, c.tag, indent + 1, localNs)).join('\n');
    const textLine = text ? `${pad}  ${escapeOrCdata(text)}\n` : '';

    return `${pad}<${tag}${attrStr}>\n${textLine}${body}\n${pad}</${tag}>`;
}

/** @param {*[]} value
 *  @param {string} tag
 *  @param {number} indent
 *  @param {string} pad
 *  @param {string} resolvedTag
 *  @param {object} ns */
function serializeArrayAsXml(value, tag, indent, pad, resolvedTag, ns) {
    if (isMixedContent(value))
        return wrapXmlMixedArray(value, pad, resolvedTag, '', indent, ns);

    return value.map(v => xqToXml(v, tag, indent, { ...ns })).join('\n');
}

/** @param {*} value
 *  @param {string} tag
 *  @param {number} indent
 *  @param {object} nsMap */
function xqToXml(value, tag, indent, nsMap) {
    const pad = indentPad(indent);
    const ns = nsMap || Object.create(null);
    const nsTag = parseKey(tag);
    const resolvedTag = resolveXmlTag(tag, ns);
    const tagNeedsNs = nsTag && !(nsTag.uri in ns);
    const tagDecl = tagNeedsNs ? mkNsDeclStr(nsTag.uri, ns) : '';

    if (value == null)
        return `${pad}<${resolvedTag}${tagDecl}/>`;
    if (isPrimitive(value))
        return wrapXmlPrimitive(value, pad, resolvedTag, tagDecl);
    if (isArray(value))
        return serializeArrayAsXml(value, tag, indent, pad, resolvedTag, ns);

    return renderXmlObject(value, pad, resolvedTag, indent, ns, nsTag, tagNeedsNs, tagDecl);
}

/** @param {object} doc
 *  @param {string|null} root
 *  @param {Function} serializeOne */
function serializeDoc(doc, root, serializeOne) {
    if (root) doc = { [root]: doc };
    if (!isPlainObject(doc)) die('non-object cannot convert');

    const docType = doc['!doctype'];
    let out = docType ? fmtDoctype(docType) + '\n' : '';

    for (const [tag, val] of Object.entries(doc)) {
        if (isDoctypeTag(tag)) continue;
        out += serializeOne(val, tag) + '\n';
    }

    return out;
}

/** @param {object} doc
 *  @param {string|null} root
 *  @param {object} nsMap */
function docToXml(doc, root, nsMap) {
    return serializeDoc(doc, root, (val, tag) => xqToXml(val, tag, 0, nsMap));
}

/** @param {object} doc
 *  @param {string|null} root */
function docToHtml(doc, root) {
    return serializeDoc(doc, root, (val, tag) => serialize(toHtmlNode(val, tag), { voidSelfClosing: true }));
}

// ======== Format: HTML ========

function isTextType(n)   { return n.type === 'text' || n.type === 'cdata'; }
function isDoctypeNode(n) { return n.type === 'directive' && n.name === '!doctype'; }
function isDoctypeTag(name) { return name === '!doctype'; }
function isTagType(n) { return n.type === 'tag' || n.type === 'script' || n.type === 'style'; }

/** @param {string} d
 *  @returns {object} */
function parseDoctype(d) {
    const m = d.match(/^!DOCTYPE\s+(\S+)(?:\s+PUBLIC\s+"([^"]*)"\s+"([^"]*)")?(?:\s+SYSTEM\s+"([^"]*)")?$/);
    if (!m) return { DOCTYPE: 'html' };

    const obj = { DOCTYPE: m[1] };
    if (m[2] !== undefined) { obj.PUBLIC = m[2]; obj.SYSTEM = m[3]; }
    if (m[4] !== undefined) { obj.SYSTEM = m[4]; }

    return obj;
}

/** @param {object} doc */
function fmtDoctype(doc) {
    let s = `<!DOCTYPE ${doc.DOCTYPE}`;

    if (doc.PUBLIC) s += ` PUBLIC "${doc.PUBLIC}" "${doc.SYSTEM}"`;
    else if (doc.SYSTEM) s += ` SYSTEM "${doc.SYSTEM}"`;
    s += '>';

    return s;
}

/** @param {object} n
 *  @returns {object|null} */
function adapt(n) {
    if (isTextType(n))
        return { nodeType: TEXT, nodeValue: n.data || '', childNodes: [] };
    if (!isTagType(n))
        return null;

    const childNodes = (n.children || []).map(adapt).filter(Boolean);
    const items = Object.entries(n.attribs || {})
        .filter(([k]) => k)
        .map(([k, v]) => ({ name: k, value: v }));

    return {
        nodeType: ELEMENT, tagName: n.name,
        attributes: items,
        childNodes,
    };
}

/** @param {object[]} elems
 *  @param {Set|null} forceSet */
function buildDocMap(elems, forceSet) {
    const doc = {};

    for (const el of elems)
        pushChild(doc, el.tagName, walk(el, forceSet));

    return doc;
}

/** @param {object[]} tags
 *  @param {Set|null} forceSet */
function buildDocResult(tags, forceSet) {
    if (isEmpty(tags)) return {};
    if (tags.length === 1)
        return { [tags[0].tagName]: walk(tags[0], forceSet) };

    return buildDocMap(tags, forceSet);
}

/** @param {string} str
 *  @param {object} opts */
function parseHtml(str, opts) {
    const forceSet = buildForceSet(opts);
    const dom = parseDocument(str);
    const doctype = dom.children.find(isDoctypeNode);
    const tags = dom.children
        .filter(isTagType)
        .map(adapt)
        .filter(Boolean);

    const result = buildDocResult(tags, forceSet);
    if (doctype) result['!doctype'] = parseDoctype(doctype.data);

    return result;
}

/** @param {object[]} children
 *  @param {*[]} items */
function toHtmlMixedItems(children, items) {
    for (const item of items) {
        if (isString(item))
            children.push({ type: 'text', data: item });
        else
            children.push(toHtmlNode(Object.values(item)[0], Object.keys(item)[0]));
    }
}

/** @param {object} value
 *  @param {object} node */
function classifyHtmlObject(value, node) {
    let text = '';

    for (const [k, v] of Object.entries(value)) {
        if (isAttr(k))
            node.attribs[k.slice(1)] = v;
        else if (isMixedKey(k))
            toHtmlMixedItems(node.children, v);
        else if (isTextContent(k))
            text = flattenText(v);
        else if (isArray(v))
            node.children.push(...v.map(item => toHtmlNode(item, k)));
        else
            node.children.push(toHtmlNode(v, k));
    }

    return text;
}

/** @param {object} node
 *  @param {*} value */
function setHtmlText(node, value) {
    const s = String(value).trim();

    if (!isEmptyString(s))
        node.children.push({ type: 'text', data: s });

    return node;
}

/** @param {object} node
 *  @param {*|*[]} value
 *  @param {string} tag */
function appendHtmlArray(node, value, tag) {
    if (isMixedContent(value))
        toHtmlMixedItems(node.children, value);
    else
        node.children.push(...value.map(v => toHtmlNode(v, tag)));

    return node;
}

/** @param {*} value
 *  @param {string} tag */
function toHtmlNode(value, tag) {
    const node = { type: 'tag', name: tag, children: [], attribs: {} };

    if (value == null) return node;
    if (isPrimitive(value)) return setHtmlText(node, value);
    if (isArray(value)) return appendHtmlArray(node, value, tag);

    const text = classifyHtmlObject(value, node);
    if (text) node.children.push({ type: 'text', data: text });

    return node;
}

// ======== Format: YAML ========

/**
 * Parse YAML string into JS objects.
 * Returns an array — one element per document (handles multi-doc with ---).
 * @param {string} str
 * @returns {object[]}
 */
function parseYaml(str) {
    let docs;
    try {
        docs = yaml.loadAll(str);
    } catch (e) {
        die(`YAML parse error: ${e.message}`);
    }

    if (docs.length === 0) return [{}];
    return docs;
}

/**
 * Serialize a JS value to YAML string.
 * @param {any} val
 * @param {object} opts
 * @param {number} [opts.indent]
 * @param {boolean} [opts.indentlessLists]
 * @param {boolean} [opts.explicitStart]
 * @param {boolean} [opts.explicitEnd]
 * @param {number} [opts.width]
 * @returns {string}
 */
function serializeYaml(val, opts) {
    return yaml.dump(val, {
        indent: opts.indent || 2,
        indentlessSequence: opts.indentlessLists || false,
        lineWidth: opts.width || 80,
        forceQuotes: false,
        quotingType: '"',
        noRefs: true,
        sortKeys: false,
    });
}

/** @param {string[]} docs
 *  @param {object} opts */
function assembleYamlDocs(docs, opts) {
    const trimmed = docs.map(d => d.replace(/\n$/, ''));
    let out;

    if (trimmed.length <= 1) {
        out = trimmed[0] || '';
    } else {
        out = trimmed.join('\n---\n');
    }

    if (opts.yamlExplicitStart)
        out = `---\n${out}`;
    if (opts.yamlExplicitEnd)
        out += '\n...';

    return out + '\n';
}

/**
 * Join serialized documents, inserting YAML multi-doc separators as needed.
 * @param {string[]} docs - serialized document strings
 * @param {string} format - 'xml', 'html', or 'yaml'
 * @param {object} opts
 * @returns {string}
 */
/** @param {string[]} docs
 *  @param {string} format
 *  @param {object} opts */
function joinSerialized(docs, format, opts) {
    if (format === 'json') return docs.join('\n') + '\n';
    if (format !== 'yaml') return docs.join('');

    return assembleYamlDocs(docs, opts);
}

// ======== Input/Output dispatch ========

const PARSE = {
    xml:  (str, opts) => {
        const { rootTag, value } = parseXml(str, opts);
        return [{ [rootTag]: value }];
    },
    html: (str, opts) => [parseHtml(str, opts)],
    yaml: (str) => parseYaml(str),
};

const SERIALIZE = {
    json: (val)      => JSON.stringify(val, null, 2),
    xml:  (val, opts) => docToXml(val, opts.xmlRoot, Object.create(null)),
    html: (val, opts) => docToHtml(val, opts.xmlRoot),
    yaml: (val, opts) => serializeYaml(val, opts),
};

// ======== Format detection ========

/**
 * @param {string} name - filename
 * @returns {string|null} format name or null
 */
function detectFormatByExt(name) {
    if (/\.ya?ml$/i.test(name)) return 'yaml';
    if (/\.html?$/i.test(name))  return 'html';
    if (/\.xml$/i.test(name))    return 'xml';
    return null;
}

/**
 * @param {string} str - input content
 * @returns {string} 'html' or 'xml' (never yaml — too ambiguous)
 */
function detectFormatByContent(str) {
    const s = str.trimStart();

    if (/^<!DOCTYPE html/i.test(s) || /^<html[\s>]/i.test(s))
        return 'html';
    return 'xml';
}

/** @param {string} input
 *  @param {string[]} files
 *  @param {object} opts */
function resolveInputFormat(input, files, opts) {
    if (opts.xmlInput)  return 'xml';
    if (opts.htmlInput) return 'html';
    if (opts.yamlInput) return 'yaml';

    if (INVOCATION_MODE.input) return INVOCATION_MODE.input;

    if (isSingleFile(files)) {
        const byExt = detectFormatByExt(files[0]);
        if (byExt) return byExt;
    }

    return detectFormatByContent(input);
}

/** @param {object} opts
 *  @returns {string|null} */
function resolveOutputFormat(opts) {
    if (opts.jsonOutput) return 'json';
    if (opts.xmlOutput)  return 'xml';
    if (opts.htmlOutput) return 'html';
    if (opts.yamlOutput) return 'yaml';

    if (INVOCATION_MODE.output) return INVOCATION_MODE.output;

    return null;
}

// ======== CLI args ========

const JQ_OPTS = {
    '--arg': 2,
    '--argjson': 2,
    '--slurpfile': 2,
    '--argfile': 2,
    '--rawfile': 2,
};

const LONG_OPTS = {
    '--xml-output': 'xmlOutput',
    '--html-output': 'htmlOutput',
    '--yaml-output': 'yamlOutput',
    '--json-output': 'jsonOutput',
    '--in-place': 'inPlace',
    '--html': 'htmlInput',
    '--xml': 'xmlInput',
    '--yaml': 'yamlInput',
    '--help': 'help',
    '--xml-root': 'xmlRoot',
    '--xml-force-list': 'xmlForceList',
    '--yaml-indent': 'yamlIndent',
    '--yaml-indentless-lists': 'yamlIndentless',
    '--yaml-explicit-start': 'yamlExplicitStart',
    '--yaml-explicit-end': 'yamlExplicitEnd',
    '--yaml-width': 'yamlWidth',
};

const VAL_OPT_HANDLERS = {
    xmlRoot:      (v, opts) => { opts.xmlRoot = v; },
    xmlForceList: (v, opts) => { opts.forceList.push(v); },
    yamlIndent:   (v, opts) => { opts.yamlIndent = parseInt(v, 10); },
    yamlWidth:    (v, opts) => { opts.yamlWidth = parseInt(v, 10); },
};

const SHORT_FLAG_MAP = {
    x: 'xmlInput',
    X: 'xmlOutput',
    h: 'htmlInput',
    H: 'htmlOutput',
    y: 'yamlInput',
    Y: 'yamlOutput',
    i: 'inPlace',
    J: 'jsonOutput',
    n: 'nullInput',
};

function parseDashArg(arg, opts) {
    if (!opts.filterSeen) {
        opts.filter = arg;
        opts.filterSeen = true;
        return;
    }
    opts.files.push(arg);
}

function parseArgs(argv) {
    const opts = {
        jsonOutput: false, xmlOutput: false, htmlOutput: false, yamlOutput: false,
        xmlInput: false, htmlInput: false, yamlInput: false,
        inPlace: false, help: false,
        nullInput: false,
        xmlRoot: null, forceList: [],
        yamlIndent: null, yamlIndentless: false,
        yamlExplicitStart: false, yamlExplicitEnd: false,
        yamlWidth: null,
        filter: '.', files: [], jqArgs: [],
        filterSeen: false,
    };

    let i = 2;
    let afterDash = false;

    while (i < argv.length) {
        const arg = argv[i];

        if (afterDash) { parseDashArg(arg, opts); i++; continue; }
        if (arg === '--') { afterDash = true; i++; continue; }
        if (isLongFlag(arg)) { i = parseLongArg(arg, i, opts, argv); continue; }
        if (isShortFlag(arg)) { parseShortFlags(arg, opts); i++; continue; }
        if (!opts.filterSeen) { opts.filter = arg; opts.filterSeen = true; i++; continue; }

        opts.files.push(arg);
        i++;
    }

    return opts;
}

/** @param {string} name
 *  @param {string|null} val
 *  @param {number} nArgs
 *  @param {number} i
 *  @param {string[]} argv
 *  @param {object} opts */
function consumeJqOpt(name, val, nArgs, i, argv, opts) {
    const consumed = [name];
    if (val !== null) consumed.push(val);

    for (let j = consumed.length; j <= nArgs; j++) {
        i++; if (i >= argv.length) die(`${name} needs ${nArgs} args`);
        consumed.push(argv[i]);
    }

    opts.jqArgs.push(...consumed);

    return i + 1;
}

/** @param {string} arg
 *  @returns {[string, string|null]} */
function splitArg(arg) {
    const eq = arg.indexOf('=');
    const name = eq >= 0 ? arg.slice(0, eq) : arg;
    const val = eq >= 0 ? arg.slice(eq + 1) : null;

    return [name, val];
}

function isNullInput(name) { return name === '--null-input'; }

function isKnownFlag(name) { return name in LONG_OPTS; }

function isJqOption(name) { return name in JQ_OPTS; }

function isArgsOption(name) {
    return name === '--args' || name === '--jsonargs';
}

function enableNullInput(arg, opts, i) {
    opts.nullInput = true;
    opts.jqArgs.push(arg);
    return i + 1;
}

function applyFlag(name, val, opts, argv, i) {
    const key = LONG_OPTS[name];

    if (!(key in VAL_OPT_HANDLERS)) {
        opts[key] = true;
        return i + 1;
    }

    const raw = val !== null ? val : argv[i + 1];
    if (raw === undefined) die(`${name} needs a value`);

    VAL_OPT_HANDLERS[key](raw, opts);

    return val !== null ? i + 1 : i + 2;
}

function consumeArgsArg(arg, val, opts, argv, i) {
    opts.jqArgs.push(arg);

    if (val === null) {
        i++;
        while (i < argv.length) opts.jqArgs.push(argv[i++]);
    }
    return i;
}

function passThroughArg(arg, opts, i) {
    opts.jqArgs.push(arg);
    return i + 1;
}

/** @param {string} arg
 *  @param {number} i
 *  @param {object} opts
 *  @param {string[]} argv */
function parseLongArg(arg, i, opts, argv) {
    const [name, val] = splitArg(arg);

    if (isNullInput(name))    return enableNullInput(arg, opts, i);
    if (isKnownFlag(name))    return applyFlag(name, val, opts, argv, i);
    if (isJqOption(name))     return consumeJqOpt(name, val, JQ_OPTS[name], i, argv, opts);
    if (isArgsOption(name))   return consumeArgsArg(arg, val, opts, argv, i);
    return passThroughArg(arg, opts, i);
}

/** @param {string} arg
 *  @param {object} opts */
function parseShortFlags(arg, opts) {
    for (const ch of arg.slice(1)) {
        if (!(ch in SHORT_FLAG_MAP)) {
            opts.jqArgs.push(`-${ch}`);
            continue;
        }

        opts[SHORT_FLAG_MAP[ch]] = true;

        if (ch === 'n') opts.jqArgs.push('-n');
    }
}

// ======== main ========

function printHelp() {
    const help = [
        `Usage: ${BIN} [options] <jq filter> [input file...]`,
        '',
        'Input format (default: auto-detect by content or extension):',
        '  -x, --xml      Force XML input',
        '  -h, --html     Force HTML input',
        '  -y, --yaml     Force YAML input',
        '',
        'Output format:',
        '  -J, --json-output  Convert JSON output to JSON (explicit)',
        '  -X, --xml-output   Convert JSON output to XML',
        '  -H, --html-output  Convert JSON output to HTML',
        '  -Y, --yaml-output  Convert JSON output to YAML',
        '',
        'XML options:',
        '      --xml-root NAME   Envelope output in element NAME',
        '      --xml-force-list ELT  Always emit array for element (repeatable)',
        '',
        'YAML options:',
        '      --yaml-indent N         Indentation width (default: 2)',
        '      --yaml-indentless-lists Use 0-indent for block sequences',
        '      --yaml-explicit-start   Always emit leading "---"',
        '      --yaml-explicit-end     Always emit trailing "..."',
        '      --yaml-width N          String wrap width (0 disables)',
        '',
        'Other:',
        '  -i, --in-place        Edit files in place (requires -X, -H, -J, or -Y)',
        '      --help            Show help',
        '',
        'Examples:',
        '  curl -s example.com | xq \'.div.content\'',
        '  cat config.xml | xq -X \'.slave.label = "new value"\'',
        '  cat config.xml | xq \'.slave.label\'',
        '  cat data.yaml | xq -Y \'.users[] | {name, email}\'',
        '',
        'See jq --help for jq filter documentation.',
    ];
    for (const line of help) console.log(line);
}

/** @param {string} f
 *  @param {object} opts
 *  @param {Function} serialize
 *  @param {string} outputFormat */
function processOneFile(f, opts, serialize, outputFormat) {
    const input = fs.readFileSync(f, 'utf-8');
    if (!input) die('no input');

    const ifmt = resolveInputFormat(input, [f], opts);

    const jsonStr = PARSE[ifmt](input, opts).map(d => JSON.stringify(d)).join('\n');
    const jqOut = runJq(jsonStr, [opts.filter, ...opts.jqArgs]);
    const docs = parseDocs(jqOut).map(d => serialize(d, opts));

    const out = joinSerialized(docs, outputFormat, opts);

    fs.writeFileSync(f, out);
}

function processInPlace(files, opts, serialize, outputFormat) {
    for (const f of files)
        processOneFile(f, opts, serialize, outputFormat);
}

function loadInput(opts) {
    if (opts.nullInput) return '';

    const input = readInput(opts.files);
    if (!input) die('no input');

    const inputFormat = resolveInputFormat(input, opts.files, opts);
    const docs = PARSE[inputFormat](input, opts);

    return docs.map(d => JSON.stringify(d)).join('\n');
}

function hasRawOutputFlag(opts) {
    return opts.jqArgs.includes('-r') || opts.jqArgs.includes('--raw-output');
}

function emitRawOutput(output) {
    if (!output.endsWith('\n')) output += '\n';
    process.stdout.write(output);
}

function emitSerializedOutput(output, opts, serialize, outputFormat) {
    const docs = parseDocs(output).map(d => serialize(d, opts));
    process.stdout.write(joinSerialized(docs, outputFormat, opts));
}

/** @param {string} output
 *  @param {object} opts
 *  @param {Function} serialize
 *  @param {string|null} outputFormat */
function emitOutput(output, opts, serialize, outputFormat) {
    if (!outputFormat || hasRawOutputFlag(opts))
        return emitRawOutput(output);

    emitSerializedOutput(output, opts, serialize, outputFormat);
}

function main() {
    const opts = parseArgs(process.argv);
    if (opts.help) return printHelp();

    const outputFormat = resolveOutputFormat(opts);
    const needsSerialize = outputFormat !== null;
    const serialize = SERIALIZE[outputFormat];

    if (opts.inPlace && !needsSerialize)
        die('--in-place needs -X, -H, or -Y');
    if (opts.inPlace && !opts.files.length)
        die('--in-place needs filenames');

    if (opts.inPlace)
        return processInPlace(opts.files, opts, serialize, outputFormat);

    const jsonStr = loadInput(opts);
    const output = runJq(jsonStr, [opts.filter, ...opts.jqArgs]);

    emitOutput(output, opts, serialize, outputFormat);
}

main();
