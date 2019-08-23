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

package main

import (
	"flag"
	"fmt"
	"github.com/spf13/pflag"
	"io/ioutil"
	vl "libvirt.org/host-validate/pkg"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	var showfacts bool
	var quiet bool
	var rulesdir string

	pflag.BoolVarP(&showfacts, "show-facts", "f", false, "Show raw fact names and values")
	pflag.BoolVarP(&quiet, "quiet", "q", false, "Don't report on fact checks")
	pflag.StringVarP(&rulesdir, "rules-dir", "r", "/usr/share/libvirt/host-validate", "Directory to load validation rules from")

	pflag.CommandLine.AddGoFlagSet(flag.CommandLine)
	pflag.Parse()
	// Convince glog that we really have parsed CLI
	flag.CommandLine.Parse([]string{})

	if len(pflag.Args()) > 1 {
		fmt.Printf("syntax: %s [OPTIONS] [DRIVER]\n", os.Args[0])
		os.Exit(1)
	}

	driver := ""
	if len(pflag.Args()) == 1 {
		driver = pflag.Args()[0]
	}

	files, err := ioutil.ReadDir(rulesdir)
	if err != nil {
		fmt.Printf("Unable to load rules from '%s': %s\n", rulesdir, err)
		os.Exit(1)
	}
	var lists []vl.FactList
	for _, file := range files {
		path := filepath.Join(rulesdir, file.Name())
		if !strings.HasSuffix(path, ".xml") {
			continue
		}
		facts, err := vl.NewFactList(path)
		if err != nil {
			fmt.Printf("Unable to load facts '%s': %s\n", path, err)
			os.Exit(1)
		}
		lists = append(lists, *facts)
	}

	var output vl.EngineOutput
	if !quiet {
		output |= vl.ENGINE_OUTPUT_REPORTS
	}
	if showfacts {
		output |= vl.ENGINE_OUTPUT_FACTS
	}

	engine := vl.NewEngine(output, driver)

	failed, err := engine.Validate(vl.MergeFactLists(lists))
	if err != nil {
		fmt.Printf("Unable to validate facts: %s\n", err)
		os.Exit(1)
	}
	if failed != 0 {
		os.Exit(2)
	}

	os.Exit(0)
}
