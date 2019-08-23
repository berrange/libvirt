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

// This defines the logic that executes the checks defined
// for each fact that is loaded. It is responsible for printing
// out messages as the facts are processed too.

import (
	"fmt"
	"golang.org/x/sys/unix"
	"io/ioutil"
	"os"
	"os/exec"
	"regexp"
	"runtime"
	"strings"
)

type Engine struct {
	Facts  map[string]string
	Errors uint
	Output EngineOutput
	Driver string
}

type EngineOutput int

const (
	// Print the raw key, value pairs for each fact set
	ENGINE_OUTPUT_FACTS = EngineOutput(1 << 0)

	// Print the human targetted reports for facts set
	ENGINE_OUTPUT_REPORTS = EngineOutput(1 << 1)
)

// Create an engine able to process a list of facts
func NewEngine(output EngineOutput, driver string) *Engine {
	engine := &Engine{}

	engine.Output = output
	engine.Facts = make(map[string]string)
	engine.Driver = driver

	return engine
}

// Set the value associated with a fact
func (engine *Engine) SetFact(name, value string) {
	engine.Facts[name] = value
	if (engine.Output & ENGINE_OUTPUT_FACTS) != 0 {
		fmt.Printf("Set fact '%s' = '%s'\n", name, value)
	}
}

func (engine *Engine) EvalExpression(expr *Expression) (bool, error) {
	if expr.Any != nil {
		for _, subexpr := range expr.Any.Expressions {
			ok, err := engine.EvalExpression(&subexpr)
			if err != nil {
				return false, err
			}
			if ok {
				return true, nil
			}
		}
		return false, nil
	} else if expr.All != nil {
		for _, subexpr := range expr.All.Expressions {
			ok, err := engine.EvalExpression(&subexpr)
			if err != nil {
				return false, err
			}
			if !ok {
				return false, nil
			}
		}
		return true, nil
	} else if expr.Fact != nil {
		val, ok := engine.Facts[expr.Fact.Name]
		if !ok {
			return false, nil
		}
		if expr.Fact.Match == "regex" {
			match, err := regexp.Match(expr.Fact.Value, []byte(val))
			if err != nil {
				return false, err
			}
			return match, nil
		} else if expr.Fact.Match == "exists" {
			return true, nil
		} else {
			return val == expr.Fact.Value, nil
		}
	} else {
		return false, fmt.Errorf("Expected expression any or all or fact")
	}
}

// Report a fact that failed to have the desired value
func (engine *Engine) Fail(fact *Fact) {
	engine.Errors++
	if fact.Report == nil {
		return
	}
	if (engine.Output & ENGINE_OUTPUT_REPORTS) != 0 {
		hint := ""
		if fact.Hint != nil {
			hint = " (" + fact.Hint.Message + ")"
		}
		if fact.Report.Level == "note" {
			fmt.Printf("\033[34mNOTE\033[0m%s\n", hint)
		} else if fact.Report.Level == "warn" {
			fmt.Printf("\033[33mWARN\033[0m%s\n", hint)
		} else {
			fmt.Printf("\033[31mFAIL\033[0m%s\n", hint)
		}
	}
}

// Report a fact that has the desired value
func (engine *Engine) Pass(fact *Fact) {
	if fact.Report == nil {
		return
	}
	if (engine.Output & ENGINE_OUTPUT_REPORTS) != 0 {
		fmt.Printf("\033[32mPASS\033[0m\n")
	}
}

func utsString(v []byte) string {
	n := 0
	for i, _ := range v {
		if v[i] == 0 {
			break
		}
		n++
	}
	return string(v[0:n])
}

// Populate the engine with values for a built-in fact
func (engine *Engine) SetValueBuiltIn(fact *Fact) error {
	var uts unix.Utsname
	err := unix.Uname(&uts)
	if err != nil {
		return err
	}

	if fact.Name == "os.kernel" {
		engine.SetFact(fact.Name, utsString(uts.Sysname[0:]))
	} else if fact.Name == "os.release" {
		engine.SetFact(fact.Name, utsString(uts.Release[0:]))
	} else if fact.Name == "os.version" {
		engine.SetFact(fact.Name, utsString(uts.Version[0:]))
	} else if fact.Name == "cpu.arch" {
		engine.SetFact(fact.Name, utsString(uts.Machine[0:]))
	} else if fact.Name == "libvirt.driver" {
		if engine.Driver != "" {
			engine.SetFact(fact.Name+"."+engine.Driver, "true")
		} else {
			if runtime.GOOS == "linux" {
				engine.SetFact(fact.Name+".qemu", "true")
				engine.SetFact(fact.Name+".lxc", "true")
				engine.SetFact(fact.Name+".parallels", "true")
			} else if runtime.GOOS == "freebsd" {
				engine.SetFact(fact.Name+".bhyve", "true")
			}
		}
	} else {
		return fmt.Errorf("Unknown built-in fact '%s'", fact.Name)
	}

	return nil
}

func (engine *Engine) SetValueBool(fact *Fact) error {
	ok, err := engine.EvalExpression(fact.Value.Bool)
	if err != nil {
		return err
	}
	got := "true"
	want := "true"
	if !ok {
		got = "false"
	}
	if fact.Report != nil && fact.Report.Pass != "" {
		want = fact.Report.Pass
	}
	engine.SetFact(fact.Name, got)
	if got == want {
		engine.Pass(fact)
	} else {
		engine.Fail(fact)
	}
	return nil
}

func unescape(val string) (string, error) {
	escapes := map[rune]string{
		'a':  "\x07",
		'b':  "\x08",
		'e':  "\x1b",
		'f':  "\x0c",
		'n':  "\x0a",
		'r':  "\x0d",
		't':  "\x09",
		'v':  "\x0b",
		'\\': "\x5c",
		'0':  "\x00",
	}
	var ret string
	escape := false
	for _, c := range val {
		if c == '\\' {
			escape = true
		} else if escape {
			unesc, ok := escapes[c]
			if !ok {
				return "", fmt.Errorf("Unknown escape '\\%c'", c)
			}
			ret += string(unesc)
			escape = false
		} else {
			ret += string(c)
		}
	}
	return ret, nil
}

func (engine *Engine) SetValueParse(fact *Fact, parse *Parse, context string, val string) error {
	if parse == nil {
		engine.SetFact(context, val)
		return nil
	}
	if parse.Whitespace == "trim" {
		val = strings.TrimSpace(val)
	}
	if parse.Scalar != nil {
		if parse.Scalar.Regex != "" {
			re, err := regexp.Compile(parse.Scalar.Regex)
			if err != nil {
				return err
			}
			matches := re.FindStringSubmatch(val)
			if parse.Scalar.Match >= uint(len(matches)) {
				val = ""
			} else {
				val = matches[parse.Scalar.Match]
			}
		}
		engine.SetFact(context, val)
	} else if parse.List != nil {
		if val == "" {
			return nil
		}
		sep, err := unescape(parse.List.Separator)
		if err != nil {
			return err
		}
		bits := strings.Split(val, sep)
		count := uint(0)
		for i, bit := range bits {
			if i < int(parse.List.SkipHead) {
				continue
			}
			if i >= (len(bits) - int(parse.List.SkipTail)) {
				continue
			}
			subcontext := fmt.Sprintf("%s.%d", context, i)
			err := engine.SetValueParse(fact, parse.List.Parse, subcontext, bit)
			if err != nil {
				return err
			}
			count++
			if count >= parse.List.Limit {
				break
			}
		}
	} else if parse.Set != nil {
		if val == "" {
			return nil
		}
		sep, err := unescape(parse.Set.Separator)
		if err != nil {
			return err
		}
		bits := strings.Split(val, sep)
		for i, bit := range bits {
			if i < int(parse.Set.SkipHead) {
				continue
			}
			if i >= (len(bits) - int(parse.Set.SkipTail)) {
				continue
			}
			if parse.Set.Regex != "" {
				re, err := regexp.Compile(parse.Set.Regex)
				if err != nil {
					return err
				}
				matches := re.FindStringSubmatch(bit)
				if parse.Set.Match >= uint(len(matches)) {
					bit = ""
				} else {
					bit = matches[parse.Set.Match]
				}
			}
			subcontext := fmt.Sprintf("%s.%s", context, bit)
			engine.SetFact(subcontext, "true")
		}
	} else if parse.Dict != nil {
		sep, err := unescape(parse.Dict.Separator)
		if err != nil {
			return err
		}
		dlm, err := unescape(parse.Dict.Delimiter)
		if err != nil {
			return err
		}
		bits := strings.Split(val, sep)
		for _, bit := range bits {
			pair := strings.SplitN(bit, dlm, 2)
			if len(pair) != 2 {
				//return fmt.Errorf("Cannot split %s value '%s' on '%s'", fact.Name, pair, parse.Dict.Delimiter)
				continue
			}
			key := strings.TrimSpace(pair[0])
			subcontext := fmt.Sprintf("%s.%s", context, key)
			err := engine.SetValueParse(fact, parse.Dict.Parse, subcontext, pair[1])
			if err != nil {
				return err
			}
		}
	} else {
		return fmt.Errorf("Expecting scalar or list or dict to parse")
	}

	return nil
}

func (engine *Engine) SetValueString(fact *Fact) error {
	val, ok := engine.Facts[fact.Value.String.Fact]
	if !ok {
		return fmt.Errorf("Fact %s not present", fact.Value.String.Fact)
	}

	return engine.SetValueParse(fact, fact.Value.String.Parse, fact.Name, string(val))
}

func (engine *Engine) SetValueFile(fact *Fact) error {
	data, err := ioutil.ReadFile(fact.Value.File.Path)
	if err != nil {
		if os.IsNotExist(err) && fact.Value.File.IgnoreMissing {
			return nil
		}
		return err
	}

	return engine.SetValueParse(fact, fact.Value.File.Parse, fact.Name, string(data))
}

func (engine *Engine) SetValueDirEnt(fact *Fact) error {
	files, err := ioutil.ReadDir(fact.Value.DirEnt.Path)
	if err != nil {
		if os.IsNotExist(err) && fact.Value.DirEnt.IgnoreMissing {
			return nil
		}
		return err
	}
	for _, file := range files {
		engine.SetFact(fmt.Sprintf("%s.%s", fact.Name, file.Name()), "true")
	}
	return nil
}

func (engine *Engine) SetValueCommand(fact *Fact) error {
	var args []string
	for _, arg := range fact.Value.Command.Args {
		args = append(args, arg)
	}
	cmd := exec.Command(fact.Value.Command.Name, args...)
	out, err := cmd.Output()
	if err != nil {
		return err
	}

	return engine.SetValueParse(fact, fact.Value.Command.Parse, fact.Name, string(out))
}

func (engine *Engine) SetValueAccess(fact *Fact) error {
	var flags uint32
	if fact.Value.Access.Check == "exists" {
		flags = 0
	} else if fact.Value.Access.Check == "readable" {
		flags = unix.R_OK
	} else if fact.Value.Access.Check == "writable" {
		flags = unix.W_OK
	} else if fact.Value.Access.Check == "executable" {
		flags = unix.X_OK
	} else {
		return fmt.Errorf("No access check type specified for %s",
			fact.Value.Access.Path)
	}
	err := unix.Access(fact.Value.Access.Path, flags)
	if err != nil {
		engine.SetFact(fact.Name, "false")
		engine.Fail(fact)
	} else {
		engine.SetFact(fact.Name, "true")
		engine.Pass(fact)
	}
	return nil
}

func (engine *Engine) ValidateFact(fact *Fact) error {
	if fact.Filter != nil {
		ok, err := engine.EvalExpression(fact.Filter)
		if err != nil {
			return err
		}
		if !ok {
			return nil
		}
	}
	if fact.Report != nil && (engine.Output&ENGINE_OUTPUT_REPORTS) != 0 {
		fmt.Printf("Checking %s...", fact.Report.Message)
	}

	if fact.Value.BuiltIn != nil {
		return engine.SetValueBuiltIn(fact)
	} else if fact.Value.Bool != nil {
		return engine.SetValueBool(fact)
	} else if fact.Value.String != nil {
		return engine.SetValueString(fact)
	} else if fact.Value.File != nil {
		return engine.SetValueFile(fact)
	} else if fact.Value.DirEnt != nil {
		return engine.SetValueDirEnt(fact)
	} else if fact.Value.Command != nil {
		return engine.SetValueCommand(fact)
	} else if fact.Value.Access != nil {
		return engine.SetValueAccess(fact)
	} else {
		return fmt.Errorf("No information provided for value in fact %s", fact.Name)
	}
}

// Validate all facts in the list, returning a count of
// any non-fatal errors encountered.
func (engine *Engine) Validate(facts FactList) (uint, error) {
	err := facts.Sort()
	if err != nil {
		return 0, err
	}
	for _, fact := range facts.Facts {
		err = engine.ValidateFact(fact)
		if err != nil {
			return 0, err
		}
	}
	return engine.Errors, nil
}
