#!/usr/bin/env bun
// pup — CSS selector HTML filter, inspired by ericchiang/pup
//
// WHAT IT DOES
//   Read HTML from stdin or file, apply CSS selectors, output filtered
//   content as HTML, text, attribute values, or JSON.
//
// USAGE
//   pup [options] [selector] [display function]
//   pup -f file.html [selector] [display function]
//
// OPTIONS
//   -f, --file FILE    Read HTML from FILE instead of stdin
//   -n, --number       Print count of matched elements
//   -i, --indent N     Indent JSON output by N spaces (default 2)
//   -h, --help         Show help
//       --version      Show version
//
// DISPLAY FUNCTIONS
//   (none)     Output matched elements as cleaned HTML
//   text{}     Print text content of matched elements (depth-first)
//   attr{KEY}  Print values of attribute KEY from matched elements
//   json{}     Output matched elements as JSON array or object
//
// EXAMPLES
//   cat file.html | pup 'div.title'
//   cat file.html | pup 'a attr{href}'
//   cat file.html | pup 'div.item json{}'
//
// EXIT CODES
//   0   Success
//   1   Parse error, IO error, selector error
//
// DEPENDENCIES
//   htmlparser2  https://github.com/fb55/htmlparser2
//   css-select   https://github.com/adrai/css-select
//   domutils     https://github.com/fb55/domutils
//
// ============================================================================
// TUTORIAL: HOW SELECTION WORKS
// ============================================================================
//
// CSS selectors pick elements from the DOM tree.  Key patterns:
//
//   TAG:       'p'         selects all <p> elements
//   CLASS:     '.intro'    selects elements with class="intro"
//   ID:        '#main'     selects element with id="main"
//   NESTED:    'div p'     selects <p> inside <div> (any depth)
//   CHILD:     'ul > li'   selects <li> directly under <ul>
//   GROUP:     'h1, a'     selects all <h1> and <a> elements
//
// Display functions format the matched elements:
//
//   html      — pretty-printed HTML, indented by DOM depth
//   text{}    — depth-first text content, trimmed per element
//   attr{KEY} — value of attribute KEY from each match
//   json{}    — each match as { tag, attribs, text }
//
// Topmost filtering: when a selector matches nested elements, only
// the outermost at each position is returned.  A parent's HTML
// already contains its children.

const hp2 = require('htmlparser2');
const css = require('css-select');
const du = require('domutils');
const fs = require('fs');

// ======== predicates ========

const DISPLAY_PATTERN = /^(text|attr|json)\{.*\}$/;

function hasDisplayFn(arg) { return DISPLAY_PATTERN.test(arg); }

function hasAttribs(el) {
    return el.attribs && Object.keys(el.attribs).length > 0;
}

function isValidIndent(n) {
    return !isNaN(n) && n >= 0;
}

function isRoot(el) {
    return el.type === 'root' || el.type === 'directive';
}

function isText(el) {
    return el.type === 'text';
}

function isComment(el) {
    return el.type === 'comment';
}

function isEmpty(seq) {
    return seq.length === 0;
}

const VOID_ELEMENTS = new Set(['br', 'hr', 'img', 'input', 'meta', 'link',
    'area', 'base', 'col', 'embed', 'source', 'track', 'wbr']);

function isVoid(tag) {
    return VOID_ELEMENTS.has(tag);
}

/** @param {Element} el
 *  @param {Set} set */
function isTopmost(el, set) {
    for (let p = el.parent; p; p = p.parent)
        if (set.has(p)) return false;
    return true;
}

// ======== content extractors ========

function elementText(el) {
    return du.textContent(el).trim();
}

function nodeText(el) {
    return el.data.trim();
}

// ======== IO ========

/** @param {string} msg */
function die(msg) {
    process.stderr.write(`pup: error: ${msg}\n`);
    process.exit(1);
}

/** @param {string|null} filePath */
function readInput(filePath) {
    try {
        return filePath
            ? fs.readFileSync(filePath, 'utf-8')
            : fs.readFileSync(0, 'utf-8');
    } catch (e) {
        die(e.message);
    }
}

// ======== CLI helpers ========

/** @param {string[]} args
 *  @param {number} i
 *  @param {string} message */
function requireArg(args, i, message) {
    if (i >= args.length) die(message);
    return args[i];
}

/** @param {string} s */
function parseIndent(s) {
    const n = parseInt(s, 10);
    return isValidIndent(n) ? n : DEFAULT_INDENT;
}

// ======== flag predicates ========

function isEndOfFlags(arg) {
    return arg === '--';
}

// ======== output formatters ========

/** @param {Element[]} elements */
function formatText(elements) {
    return elements
        .map(el => elementText(el))
        .join('\n') + '\n';
}

/** @param {Element[]} elements
 *  @param {string} key */
function formatAttr(elements, key) {
    return elements
        .map(el => du.getAttributeValue(el, key) ?? '')
        .join('\n') + '\n';
}

/** @param {Element} el */
function elementToJson(el) {
    const text = elementText(el);
    const obj = {};

    if (hasAttribs(el))
        Object.assign(obj, el.attribs);

    obj.tag = el.name || el.type || '?';

    if (text)
        obj.text = text;

    return obj;
}

/** @param {Element[]} elements
 *  @param {number} indent */
function formatJson(elements, indent) {
    const arr = elements.map(elementToJson);

    return JSON.stringify(arr, null, indent) + '\n';
}

// ======== HTML formatting ========

function indentAt(depth) {
    return ' '.repeat(depth);
}

/** @param {object[]} children
 *  @param {number} depth */
function renderChildren(children, depth) {
    return children.map(c => prettyHtml(c, depth)).join('');
}

/** @param {Element} el */
function tagAttrs(el) {
    if (!hasAttribs(el)) return '';

    return Object.entries(el.attribs)
        .map(([k, v]) => ` ${k}="${v}"`)
        .join('');
}

/** @param {object} el
 *  @param {number} depth */
function textNodeHtml(el, depth) {
    const text = nodeText(el);
    if (!text) return '';

    const pad = indentAt(depth);
    return `${pad}${text}\n`;
}

/** @param {object} el
 *  @param {number} depth */
function commentHtml(el, depth) {
    const pad = indentAt(depth);
    return `${pad}<!--${el.data}-->\n`;
}

/** @param {string} tag
 *  @param {string} attrs
 *  @param {number} depth */
function voidElementHtml(tag, attrs, depth) {
    const pad = indentAt(depth);
    return `${pad}<${tag}${attrs}>\n`;
}

/** @param {string} tag
 *  @param {string} attrs
 *  @param {number} depth */
function emptyElementHtml(tag, attrs, depth) {
    const pad = indentAt(depth);
    return `${pad}<${tag}${attrs}></${tag}>\n`;
}

/** @param {string} tag
 *  @param {string} attrs
 *  @param {object[]} children
 *  @param {number} depth */
function elementTreeHtml(tag, attrs, children, depth) {
    const pad = indentAt(depth);

    return `${pad}<${tag}${attrs}>\n`
        + renderChildren(children, depth + 1)
        + `${pad}</${tag}>\n`;
}

/** @param {object} el
 *  @param {number} depth */
function prettyHtml(el, depth) {
    // Non-element nodes
    if (isRoot(el))
        return renderChildren(el.children || [], depth);
    if (isText(el))
        return textNodeHtml(el, depth);
    if (isComment(el))
        return commentHtml(el, depth);

    // Element nodes
    const attrs = tagAttrs(el);
    if (isVoid(el.name))
        return voidElementHtml(el.name, attrs, depth);

    const children = el.children || [];
    if (isEmpty(children))
        return emptyElementHtml(el.name, attrs, depth);

    return elementTreeHtml(el.name, attrs, children, depth);
}

/** @param {Element[]} elements */
function topmost(elements) {
    const set = new Set(elements);

    return elements.filter(el => isTopmost(el, set));
}

/** @param {Element[]} elements */
function formatHtml(elements) {
    return topmost(elements).map(el => prettyHtml(el, 0)).join('');
}

// Hoisted dispatcher — avoids rebuilding per call.
const FORMATTERS = {
    text: (els, _arg, _indent) => formatText(els),
    attr: (els, arg, _indent) => formatAttr(els, arg),
    json: (els, _arg, indent) => formatJson(els, indent),
    html: (els, _arg, _indent) => formatHtml(els),
};

/** @param {string} displayFn
 *  @param {Element[]} elements
 *  @param {string} displayArg
 *  @param {number} indent */
function formatOutput(displayFn, elements, displayArg, indent) {
    return FORMATTERS[displayFn](elements, displayArg, indent);
}

// ======== CLI flag parsing ========

const DEFAULT_INDENT = 2;

/** @param {string[]} args */
function parseFlags(args) {
    const opts = {
        filePath: null, showCount: false, indent: DEFAULT_INDENT,
        help: false, version: false, positional: [],
    };
    let i = 0;

    while (canParseFlags(i, args, opts)) {
        const arg = args[i];
        if (isEndOfFlags(arg)) { i++; break; }
        if (!isFlag(arg)) break;

        i = applyFlag(arg, args, i, opts);
    }

    opts.positional = args.slice(i);
    return opts;
}

/** @param {number} i
 *  @param {string[]} args
 *  @param {object} opts */
function canParseFlags(i, args, opts) {
    return i < args.length && !opts.help && !opts.version;
}

function isFlag(arg) {
    return arg.startsWith('-');
}

const FLAG_HANDLERS = {
    file: (args, i, opts) => {
        i++;
        opts.filePath = requireArg(args, i, '--file needs a path');
        return i + 1;
    },
    number: (_args, i, opts) => {
        opts.showCount = true;
        return i + 1;
    },
    indent: (args, i, opts) => {
        i++;
        opts.indent = parseIndent(requireArg(args, i, '--indent needs a number'));
        return i + 1;
    },
    help: (_args, i, opts) => {
        opts.help = true;
        return i + 1;
    },
    version: (_args, i, opts) => {
        opts.version = true;
        return i + 1;
    },
};

const FLAG_ALIASES = {
    '-f': 'file',
    '--file': 'file',
    '-n': 'number',
    '--number': 'number',
    '-i': 'indent',
    '--indent': 'indent',
    '-h': 'help',
    '--help': 'help',
    '--version': 'version',
};

/** @param {string} arg
 *  @param {string[]} args
 *  @param {number} i
 *  @param {object} opts */
function applyFlag(arg, args, i, opts) {
    const key = FLAG_ALIASES[arg];
    if (!key) die(`unknown flag ${arg}`);

    return FLAG_HANDLERS[key](args, i, opts);
}

// ======== display function parsing ========

/** @param {string} arg
 *  @returns {{fn:string,arg:string}|null} */
function parseDisplayFn(arg) {
    const m = arg.match(/^(text|attr|json)\{(.*)\}$/);
    if (!m) return null;
    return { fn: m[1], arg: m[2] };
}

/** @param {string[]} remaining
 *  @returns {{selector:string,displayFn:string,displayArg:string}} */
function parseDisplay(remaining) {
    const tokens = remaining.flatMap(arg => arg.split(/\s+/));

    if (isEmpty(tokens))
        return { selector: null, displayFn: 'html', displayArg: '' };

    const last = tokens[tokens.length - 1];

    if (hasDisplayFn(last)) {
        const parsed = parseDisplayFn(last);
        const selector = tokens.slice(0, -1).join(' ') || '*';
        return { selector, displayFn: parsed.fn, displayArg: parsed.arg };
    }

    return { selector: tokens.join(' '), displayFn: 'html', displayArg: '' };
}

// ======== help/version ========

const VERSION = '0.1.0';

const HELP_TEXT = [
    'Usage: pup [options] [selector] [display function]',
    '',
    'Options:',
    '  -f, --file FILE    Read HTML from FILE instead of stdin',
    '  -n, --number       Print count of matched elements',
    '  -i, --indent N     Indent JSON by N spaces (default 2)',
    '  -h, --help         Show this help',
    '      --version      Show version',
    '',
    'Display functions:',
    '  (none)     Output matched elements as HTML',
    '  text{}     Print text content',
    '  attr{KEY}  Print attribute values',
    '  json{}     Output as JSON',
    '',
    'Examples:',
    "  cat file.html | pup 'h1'",
    "  cat file.html | pup 'a attr{href}'",
    "  cat file.html | pup 'div.item json{}'",
];

function printHelp() {
    for (const line of HELP_TEXT)
        console.error(line);
}

function printVersion() {
    console.error(`pup.js ${VERSION}`);
}

// ======== DOM helpers ========

/** @param {string} html */
function parseHtml(html) {
    try { return hp2.parseDocument(html); }
    catch (e) { die(`parse error: ${e.message}`); }
}

/** @param {string} selector
 *  @param {object} dom */
function select(selector, dom) {
    try { return css.selectAll(selector, dom); }
    catch (e) { die(`selector error: ${e.message}`); }
}

// ======== main ========

function main() {
    const args = process.argv.slice(2);
    const flags = parseFlags(args);

    // Meta commands
    if (flags.help)
        return printHelp();
    if (flags.version)
        return printVersion();

    // Parse input
    const display = parseDisplay(flags.positional);
    const html = readInput(flags.filePath);
    const dom = parseHtml(html);
    const elements = display.selector ? select(display.selector, dom) : [dom];

    // Count or empty edge cases
    if (flags.showCount)
        return console.log(String(elements.length));
    if (isEmpty(elements))
        return;

    // Format and output
    const output = formatOutput(display.displayFn, elements, display.displayArg, flags.indent);
    process.stdout.write(output);
}

main();
