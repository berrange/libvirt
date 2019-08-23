/*
 * This file is part of the libvirt project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 */

package pkg

import (
	"encoding/xml"
	"fmt"
	"io"
	"io/ioutil"
	"strings"
)

// A list of all the facts we are going to validate
type FactList struct {
	XMLName xml.Name `xml:"https://libvirt.org/xml/virt-host-validate/facts/1.0 factlist"`

	Facts []*Fact `xml:"fact"`
}

// A fact is a description of a single piece of information
// we wish to check. Conceptually a fact is simply a plain
// key, value pair where both parts are strings.
//
// Every fact has a name which is a dot separated list of
// strings, eg 'cpu.family.arm'. By convention the dots
// are forming an explicit hierarchy, so a common prefix
// on names is used to group related facts.
//
// Optionally a report can be associated with a fact
// This is a freeform string intended to be read by
// humans, eg 'hardware virt possible'
//
// If a report is given, there can also be an optional
// hint given, which is used when a fact fails to match
// some desired condition. This is another freeform string
// intended to be read by humans, eg
// 'enable cpuset cgroup controller in Kconfig'
//
// The optional filter is an expression that can be used
// to skip the processing of this fact when certain
// conditions are not met. eg, a filter might skip
// the checking of cgroups when the os kernel is not "linux"
//
// Finally there is a mandatory value. This defines how
// to extract the value for setting the fact.
//
//
//
type Fact struct {
	Name   string      `xml:"name,attr"`
	Report *Report     `xml:"report"`
	Hint   *Report     `xml:"hint"`
	Filter *Expression `xml:"filter"`
	Value  Value       `xml:"value"`
}

// A report is a message intended to be targetted at humans
//
// The message can be an arbitrary string, informing them
// of some relevant information
//
// The level is one of 'warn' or 'note' or 'error', with
// 'error' being assumed if no value is given
type Report struct {
	Message string `xml:"message,attr"`
	Level   string `xml:"level,attr,omitempty"`
}

// An expression is used to evaluate some complex conditions
//
// Expressions can be simple, comparing a single fact to
// some desired match.
//
// Expressions can be compound, requiring any or all of a
// list of sub-expressions to evaluate to true.
type Expression struct {
	Any  *ExpressionCompound `xml:"any"`
	All  *ExpressionCompound `xml:"all"`
	Fact *ExpressionFact     `xml:"fact"`
}

// A compound expression is simply a list of expressions
// to be evaluated
type ExpressionCompound struct {
	Expressions []Expression `xml:"-"`
}

// A fact expression defines a rule that compares the
// value associated with the fact, to some desired
// match.
//
// The name gives the name of the fact to check
//
// The semantics of value vary according to the match
// type
//
// If the match type is 'regex', then the value must
// match against the declared regular expression.
//
// If the match type is 'exists', the value is ignored
// and the fact must simply exist.
//
// If the match type is not set, then a plain string
// equality comparison is done
type ExpressionFact struct {
	Name  string `xml:"name,attr"`
	Value string `xml:"value,attr,omitempty"`
	Match string `xml:"match,attr,omitempty"`
}

// A value defines the various data sources for
// setting a fact's value. Only one of the data
// sources is permitted to be non-nil for each
// fact
//
// A builtin value is one of the standard facts
// defined in code.
//
// A bool value is one set to 'true' or 'false'
// depending on the results of evaluating an
// expression. It is user error to construct
// an expression which is self-referential
//
// A string value is one set by parsing the
// the value of another fact. A typical use
// case would be to split a string based on
// a whitespace separator
//
// A file value is one set by parsing the
// contents of a file on disk
//
// A dirent value results in creation of
// many facts, one for each directory entry
// seen
//
// An access value is one that sets a value
// to 'true' or 'false' depending on the
// permissions of a file
//
// A command value is one set by parsing
// the output of a command's stdout
type Value struct {
	BuiltIn *ValueBuiltIn `xml:"builtin"`
	Bool    *Expression   `xml:"bool"`
	String  *ValueString  `xml:"string"`
	File    *ValueFile    `xml:"file"`
	DirEnt  *ValueDirEnt  `xml:"dirent"`
	Access  *ValueAccess  `xml:"access"`
	Command *ValueCommand `xml:"command"`
}

// Valid built-in fact names are
//  - os.{kernel,release,version} and cpu.arch,
//    set from uname() syscall results
//  - libvirt.driver set from a command line arg
type ValueBuiltIn struct {
}

// Sets a value from a command.
//
// The name is the binary command name, either
// unqualified and resolved against $PATH, or
// or fully qualified
//
// A command can be given an arbitray set of
// arguments when run
//
// By default the entire contents of stdout
// will be set as the fact value.
//
// It is possible to instead parse the stdout
// data to extract interesting pieces of information
// from it
type ValueCommand struct {
	Name  string            `xml:"name,attr"`
	Args  []ValueCommandArg `xml:"arg"`
	Parse *Parse            `xml:"parse"`
}

type ValueCommandArg struct {
	Val string `xml:"val,attr"`
}

// Sets a value from a file contents
//
// The path is the fully qualified filename path
//
// By default the entire contents of the file
// will be set as the fact value.
//
// It is possible to instead parse the file
// data to extract interesting pieces of information
// from it
type ValueFile struct {
	Path          string `xml:"path,attr"`
	Parse         *Parse `xml:"parse"`
	IgnoreMissing bool   `xml:"ignoreMissing,attr,omitempty"`
}

// Sets a value from another fact
//
// The fact is the name of the other fact
//
// By default the entire contents of the other fact
// will be set as the fact value.
//
// More usually though the other fact value will be
// parsed to extract interesting pieces of information
// from it
type ValueString struct {
	Fact  string `xml:"fact,attr"`
	Parse *Parse `xml:"parse"`
}

// Sets a value from a list of directory entries
//
// The path is the fully qualified path of the directory
//
// By default an error will be raised if the directory
// does not exist. Typically a filter rule would be
// desired to skip processing of the fact in cases
// where it is known the directory may not exist.
//
// If filters are not practical though, missing directory
// can be made non-fatal
type ValueDirEnt struct {
	Path          string `xml:"path,attr"`
	IgnoreMissing bool   `xml:"ignoreMissing,attr,omitempty"`
}

// Sets a value from the access permissions of a file
//
// The path is the fully qualified path of the file
//
// The check can be one of the strings 'readable',
// 'writable' or 'executable'.
type ValueAccess struct {
	Path  string `xml:"path,attr"`
	Check string `xml:"check,attr"`
}

// The parse object defines a set of rules for
// parsing strings to extract interesting
// pieces of data
//
// The optional whitespace attribute can be set to
// 'trim' to cause leading & trailing whitespace to
// be removed before further processing
//
// To extract a single data item from the string,
// the scalar parsing rule can be used
//
// To extract an ordered list of data items from
// the string, the list parsing rule can be used
//
// To extract an unordered list of data items,
// with duplicates excluded, the set parsing rule
// can be used
//
// To extract a list of key, value pairs from the
// string, the dict parsing rule can be used
type Parse struct {
	Whitespace string       `xml:"whitespace,attr,omitempty"`
	Scalar     *ParseScalar `xml:"-"`
	List       *ParseList   `xml:"-"`
	Set        *ParseSet    `xml:"-"`
	Dict       *ParseDict   `xml:"-"`
}

// Parsing a string to extract a single data item
// using a regular expression.
//
// The regular expression should contain at least
// one capturing group. The match attribute indicates
// which capturing group will be used to set the
// fact value.
type ParseScalar struct {
	Regex string `xml:"regex,attr,omitempty"`
	Match uint   `xml:"match,attr,omitempty"`
}

// Parsing a string to extract an ordered list of
// data items
//
// The separator declares the boundary on which
// the string will be split
//
// The skip head attribute should be non-zero if
// any leading elements in the list are to be
// discarded. This is typically useful if the
// list contains a header/label as the first
// entry
//
// The skip tail attribute should be non-zero if
// any trailing elements in the list are to be
// discarded
//
// The limit attribute sets an upper bound on
// the number of elements that will be kept in
// the list. It is applied after discarding any
// leading or trailing elements.
//
// Each element in the list is then itself parsed
type ParseList struct {
	Separator string `xml:"separator,attr"`
	SkipHead  uint   `xml:"skiphead,attr,omitempty"`
	SkipTail  uint   `xml:"skiptail,attr,omitempty"`
	Limit     uint   `xml:"limit,attr,omitempty"`
	Parse     *Parse `xml:"parse"`
}

// Parsing a string to extract an unordered list of
// data items with duplicates removed
//
// The separator declares the boundary on which
// the string will be split
//
// The skip head attribute should be non-zero if
// any leading elements in the list are to be
// discarded. This is typically useful if the
// list contains a header/label as the first
// entry
//
// The skip tail attribute should be non-zero if
// any trailing elements in the list are to be
// discarded
//
// Each element is then parsed using a regular
// expression
//
// The regular expression should contain at least
// one capturing group. The match attribute indicates
// which capturing group will be used to set the
// fact value.
type ParseSet struct {
	Separator string `xml:"separator,attr"`
	SkipHead  uint   `xml:"skiphead,attr,omitempty"`
	SkipTail  uint   `xml:"skiptail,attr,omitempty"`
	Regex     string `xml:"regex,attr,omitempty"`
	Match     uint   `xml:"match,attr,omitempty"`
}

// Parsing a string to extract an unordered list of
// data items with duplicates removed
//
// The separator declares the boundary on which
// the string will be split to acquire the list
// of pairs
//
// The delimiter declares the boundary to separate
// the key from the value
//
// The value is then further parsed with the declared
// rules
type ParseDict struct {
	Separator string `xml:"separator,attr"`
	Delimiter string `xml:"delimiter,attr"`
	Parse     *Parse `xml:"parse"`
}

func getAttr(attrs []xml.Attr, name string) (string, bool) {
	for _, attr := range attrs {
		if attr.Name.Local == name {
			return attr.Value, true
		}
	}
	return "", false
}

type parse Parse

type parseScalar struct {
	ParseScalar
	parse
}

type parseList struct {
	ParseList
	parse
}

type parseSet struct {
	ParseSet
	parse
}

type parseDict struct {
	ParseDict
	parse
}

// Custom XML generator which ensures that only one of
// the parse rules will be output
func (p *Parse) MarshalXML(e *xml.Encoder, start xml.StartElement) error {
	start.Name.Local = "parse"
	if p.List != nil {
		start.Attr = append(start.Attr, xml.Attr{
			xml.Name{Local: "format"}, "list",
		})
		pl := parseList{}
		pl.parse = parse(*p)
		pl.ParseList = *p.List
		return e.EncodeElement(pl, start)
	} else if p.Set != nil {
		start.Attr = append(start.Attr, xml.Attr{
			xml.Name{Local: "format"}, "set",
		})
		ps := parseSet{}
		ps.parse = parse(*p)
		ps.ParseSet = *p.Set
		return e.EncodeElement(ps, start)
	} else if p.Dict != nil {
		start.Attr = append(start.Attr, xml.Attr{
			xml.Name{Local: "format"}, "dict",
		})
		pd := parseDict{}
		pd.parse = parse(*p)
		pd.ParseDict = *p.Dict
		return e.EncodeElement(pd, start)
	} else if p.Scalar != nil {
		start.Attr = append(start.Attr, xml.Attr{
			xml.Name{Local: "format"}, "scalar",
		})
		ps := parseScalar{}
		ps.parse = parse(*p)
		ps.ParseScalar = *p.Scalar
		return e.EncodeElement(ps, start)
	} else {
		return fmt.Errorf("Either ParseList or ParseDict must be non-nil")
	}
}

// Custom XML parser which ensures that only one of
// the parse rules will be filled in
func (p *Parse) UnmarshalXML(d *xml.Decoder, start xml.StartElement) error {
	format, ok := getAttr(start.Attr, "format")
	if !ok {
		return fmt.Errorf("Missing format attribute on <parse>")
	}
	if format == "list" {
		pl := parseList{}
		err := d.DecodeElement(&pl, &start)
		if err != nil {
			return err
		}
		*p = Parse(pl.parse)
		p.List = &pl.ParseList
		return nil
	} else if format == "set" {
		ps := parseSet{}
		err := d.DecodeElement(&ps, &start)
		if err != nil {
			return err
		}
		*p = Parse(ps.parse)
		p.Set = &ps.ParseSet
		return nil
	} else if format == "dict" {
		pd := parseDict{}
		err := d.DecodeElement(&pd, &start)
		if err != nil {
			return err
		}
		*p = Parse(pd.parse)
		p.Dict = &pd.ParseDict
		return nil
	} else if format == "scalar" {
		ps := parseScalar{}
		err := d.DecodeElement(&ps, &start)
		if err != nil {
			return err
		}
		*p = Parse(ps.parse)
		p.Scalar = &ps.ParseScalar
		return nil
	} else {
		return fmt.Errorf("Unknown format '%s' attribute on <parse>", format)
	}
}

// Custom XML generator which ensures that only one of
// the expression rules will be output
func (m *ExpressionCompound) MarshalXML(e *xml.Encoder, start xml.StartElement) error {
	e.EncodeToken(start)
	for _, match := range m.Expressions {
		if match.Any != nil {
			el := xml.StartElement{
				Name: xml.Name{Local: "any"},
			}
			e.EncodeElement(match.Any, el)
		} else if match.All != nil {
			el := xml.StartElement{
				Name: xml.Name{Local: "all"},
			}
			e.EncodeElement(match.All, el)
		} else if match.Fact != nil {
			el := xml.StartElement{
				Name: xml.Name{Local: "fact"},
			}
			e.EncodeElement(match.Fact, el)
		} else {
			return fmt.Errorf("Expected Any or All or Fact to be set")
		}
	}
	e.EncodeToken(start.End())
	return nil
}

// Custom XML parser which ensures that only one of
// the expression rules will be filled in
func (m *ExpressionCompound) UnmarshalXML(d *xml.Decoder, start xml.StartElement) error {
	for {
		tok, err := d.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}

		switch tok := tok.(type) {
		case xml.StartElement:
			el := Expression{}
			if tok.Name.Local == "any" {
				el.Any = &ExpressionCompound{}
				err = d.DecodeElement(el.Any, &tok)
				if err != nil {
					return err
				}
			} else if tok.Name.Local == "all" {
				el.All = &ExpressionCompound{}
				err = d.DecodeElement(el.All, &tok)
				if err != nil {
					return err
				}
			} else if tok.Name.Local == "fact" {
				el.Fact = &ExpressionFact{}
				err = d.DecodeElement(el.Fact, &tok)
				if err != nil {
					return err
				}
			}
			m.Expressions = append(m.Expressions, el)
		}
	}
	return nil
}

// Helper for parsing a string containing an XML
// doc defining a list of facts
func (f *FactList) Unmarshal(doc string) error {
	return xml.Unmarshal([]byte(doc), f)
}

// Helper for formatting a string to contain an
// XML doc for the list of facts
func (f *FactList) Marshal() (string, error) {
	doc, err := xml.MarshalIndent(f, "", "  ")
	if err != nil {
		return "", err
	}
	return string(doc), nil
}

// Create a new fact list, loading from the
// specified plain file
func NewFactList(filename string) (*FactList, error) {
	xmlstr, err := ioutil.ReadFile(filename)
	if err != nil {
		return nil, err
	}

	facts := &FactList{}
	err = facts.Unmarshal(string(xmlstr))
	if err != nil {
		return nil, err
	}

	return facts, nil
}

// Used to ensure that no fact has a name which is a sub-string of
// another fact.
func validateNames(names map[string]*Fact) error {
	for name, _ := range names {
		bits := strings.Split(name, ".")
		subname := ""
		for _, bit := range bits[0 : len(bits)-1] {
			if subname == "" {
				subname = bit
			} else {
				subname = subname + "." + bit
			}
			_, ok := names[subname]

			if ok {
				return fmt.Errorf("Fact name '%s' has fact '%s' as a substring",
					name, subname)
			}
		}
	}

	return nil
}

// Identify the name of the corresponding fact that
// is referenced, by chopping off suffixes until a
// match is found
func findFactReference(names map[string]*Fact, name string) (string, error) {
	bits := strings.Split(name, ".")
	subname := ""
	for _, bit := range bits {
		if subname == "" {
			subname = bit
		} else {
			subname = subname + "." + bit
		}
		_, ok := names[subname]
		if ok {
			return subname, nil
		}
	}

	return "", fmt.Errorf("Cannot find fact providing %s", name)
}

// Build up a list of dependant facts referenced by an expression
func addDepsExpr(deps *map[string][]string, names map[string]*Fact, fact *Fact, expr *Expression) error {
	if expr.Any != nil {
		for _, sub := range expr.Any.Expressions {
			err := addDepsExpr(deps, names, fact, &sub)
			if err != nil {
				return err
			}
		}
	} else if expr.All != nil {
		for _, sub := range expr.All.Expressions {
			err := addDepsExpr(deps, names, fact, &sub)
			if err != nil {
				return err
			}
		}
	} else if expr.Fact != nil {
		ref, err := findFactReference(names, expr.Fact.Name)
		if err != nil {
			return err
		}
		entries, _ := (*deps)[fact.Name]
		entries = append(entries, ref)
		(*deps)[fact.Name] = entries
	}
	return nil
}

// Build up a list of dependancies between facts
func addDeps(deps *map[string][]string, names map[string]*Fact, fact *Fact) error {
	if fact.Filter != nil {
		err := addDepsExpr(deps, names, fact, fact.Filter)
		if err != nil {
			return err
		}
	}
	if fact.Value.Bool != nil {
		err := addDepsExpr(deps, names, fact, fact.Value.Bool)
		if err != nil {
			return err
		}
	}
	if fact.Value.String != nil {
		ref, err := findFactReference(names, fact.Value.String.Fact)
		if err != nil {
			return err
		}
		entries, _ := (*deps)[fact.Name]
		entries = append(entries, ref)
		(*deps)[fact.Name] = entries
	}
	return nil
}

// Perform a topological sort on facts so that they can be
// processed in the order required to satisfy dependancies
// between facts
func (facts *FactList) Sort() error {
	deps := make(map[string][]string)
	names := make(map[string]*Fact)

	var remaining []string
	for _, fact := range facts.Facts {
		deps[fact.Name] = []string{}
		names[fact.Name] = fact
		remaining = append(remaining, fact.Name)
	}

	err := validateNames(names)
	if err != nil {
		return err
	}

	for _, fact := range facts.Facts {
		err = addDeps(&deps, names, fact)
		if err != nil {
			return err
		}
	}

	var sorted []string
	done := make(map[string]bool)
	for len(remaining) > 0 {
		prev_done := len(done)
		var skipped []string
		for _, fact := range remaining {
			using, ok := deps[fact]
			if !ok {
				done[fact] = true
				sorted = append(sorted, fact)
			} else {
				unsolved := false
				for _, entry := range using {
					_, ok := done[entry]
					if !ok {
						unsolved = true
						break
					}
				}
				if !unsolved {
					sorted = append(sorted, fact)
					done[fact] = true
				} else {
					skipped = append(skipped, fact)
				}
			}
		}

		if len(done) == prev_done {
			return fmt.Errorf("Cycle detected in facts")
		}

		remaining = skipped
	}

	var newfacts []*Fact
	for _, name := range sorted {
		newfacts = append(newfacts, names[name])
	}

	facts.Facts = newfacts

	return nil
}

// Create a new fact list that contains all the facts
// from the passed in list of fact lists
func MergeFactLists(lists []FactList) FactList {
	var allfacts []*Fact
	for _, list := range lists {
		for _, fact := range list.Facts {
			allfacts = append(allfacts, fact)
		}
	}

	facts := FactList{}
	facts.Facts = allfacts
	return facts
}
