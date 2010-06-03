/*--
    Document.cpp  

    This file is part of the Cornucopia curve sketching library.
    Copyright (C) 2010 Ilya Baran (ibaran@mit.edu)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "Document.h"
#include "MainView.h"
#include "ParamWidget.h"
#include "Polyline.h"
#include "Fitter.h"
#include "ScrollScene.h"
#include "SceneItem.h"

#include <QFileDialog>
#include <QFile>
#include <QDataStream>
#include <QTextStream>
#include <QMessageBox>
#include <QScriptEngine>
#include <QScriptValueIterator>

using namespace std;
using namespace Eigen;

Document::Document(MainView *view)
    : QObject(view), _view(view), _sketchIdx(0)
{
}

void Document::curveDrawn(Cornu::PolylineConstPtr polyline)
{
    Sketch sketch;
    sketch.pts = polyline;
    sketch.name = _getNextSketchName();
    sketch.params = _view->paramWidget()->parameters();

    _sketches.push_back(sketch);
    Cornu::Fitter fitter;
    fitter.setOriginalSketch(sketch.pts);
    fitter.setParams(sketch.params);
    fitter.run();
    
    _view->scene()->addItem(new CurveSceneItem(fitter.finalOutput(), sketch.name));
}

void Document::refitLast()
{
    if(_sketches.empty())
        return;
    Sketch last = _sketches.back();
    _sketches.pop_back();
    _view->scene()->clearGroups(last.name);
    curveDrawn(last.pts);
}

void Document::clearAll()
{
    _view->scene()->clearGroups("");
    _sketches.clear();
    _sketchIdx = 0;
}

void Document::open()
{
    _readFile("Open Curve", true);
}

void Document::insert()
{
    _readFile("Insert Curve", false);
}

bool Document::_readFile(const QString &message, bool clear) //returns true on success
{
    vector<Document::Sketch> sketches;

    QString fileName = QFileDialog::getOpenFileName(_view, message,
                        "",
                        "Cornucopia files (*.cnc *.pts)");

    if(fileName.isEmpty())
        return false;

    bool cnc = fileName.toLower().endsWith(".cnc");
    if(!cnc && !fileName.toLower().endsWith(".pts"))
    {
        QMessageBox::critical(_view, "Error", QString("Unrecognized extension"));
        return false;
    }

    QFile file(fileName);
    if(!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(_view, "Error", QString("Could not open file for read: ") + fileName);
        return false;
    }

    if(cnc)
    {
        QTextStream in(&file);
        sketches = _readNative(in);
        if(sketches.empty())
        {
            QMessageBox::critical(_view, "Error", QString("Could not read the file: ") + fileName);
            return false;
        }
    }
    else //pts
    {
        QDataStream in(&file);

        Cornu::PolylineConstPtr newPoly = _readPts(in);
        if(newPoly)
        {
            sketches.push_back(Sketch());
            sketches.back().name = _getNextSketchName();
            sketches.back().pts = newPoly;
        }
        else
        {
            QMessageBox::critical(_view, "Error", QString("Could not read the file: ") + fileName);
            return false;
        }
    }

    if(clear)
        clearAll();

    for(int i = 0; i < (int)sketches.size(); ++i)
    {
        _sketches.push_back(sketches[i]);
        Cornu::Fitter fitter;
        fitter.setOriginalSketch(sketches[i].pts);
        fitter.setParams(sketches[i].params);
        fitter.run();
    
        _view->scene()->addItem(new CurveSceneItem(fitter.finalOutput(), sketches[i].name));
    }

    return true;
}

Cornu::PolylineConstPtr Document::_readPts(QDataStream &stream)
{
    unsigned int sz = 0;
    stream >> sz;
    if(stream.atEnd() || sz > 10000)
        return Cornu::PolylineConstPtr();

    Cornu::VectorC<Vector2d> pt(sz, Cornu::NOT_CIRCULAR);
    for(int i = 0; i < pt.size(); ++i) {
        if(stream.atEnd())
            return Cornu::PolylineConstPtr();
        stream >> pt[i][0] >> pt[i][1];
    }

    return new Cornu::Polyline(pt);
}

void Document::_writePts(QDataStream &stream, Cornu::PolylineConstPtr curve)
{
    stream << curve->pts().size();
    for(int i = 0; i < curve->pts().size(); ++i)
        stream << curve->pts()[i][0] << curve->pts()[i][1];
}

vector<Document::Sketch> Document::_readNative(QTextStream &stream)
{
    QString contents = stream.readAll();

    QScriptValue all; 
    QScriptEngine engine;
    all = engine.evaluate(contents); //parse the JSON

    vector<Sketch> out;

    if(!all.isArray())
        return out;

    QScriptValueIterator it(all);
    while (it.hasNext()) {
        it.next();
        QScriptValue curSketch = it.value();

        Sketch cur;

        //read the points
        QScriptValue pts = curSketch.property("pts");
        if(!pts.isValid() || !pts.isArray() || pts.property("length").toInt32() % 2 != 0)
            return out;

        Cornu::VectorC<Vector2d> readPts;
        QScriptValueIterator it2(pts);
        while(it2.hasNext())
        {
            it2.next();
            double x = it2.value().toNumber();
            it2.next();
            double y = it2.value().toNumber();
            readPts.push_back(Vector2d(x, y));
        }
        cur.pts = new Cornu::Polyline(readPts);

        //read the parameters
        QScriptValueIterator it3(curSketch);
        while(it3.hasNext())
        {
            it3.next();
            if(!it3.value().isNumber())
                continue;
            
            QByteArray nameArray = it3.name().toAscii();
            std::string name(nameArray.constData(), nameArray.length());
            double value = it3.value().toNumber();

            //check parameters
            const vector<Cornu::Parameters::Parameter> &params = Cornu::Parameters::parameters();
            for(int i = 0; i < (int)params.size(); ++i)
            {
                if(params[i].typeName == name)
                {
                    cur.params.set(params[i].type, value);
                    break;
                }
            }

            //check algorithms
            for(int i = 0; i < Cornu::NUM_ALGORITHM_STAGES; ++i)
            {
                if(name == Cornu::AlgorithmBase::get((Cornu::AlgorithmStage)i, 0)->stageName())
                {
                    cur.params.setAlgorithm(i, (int)value);
                    break;
                }
            }
        }

        cur.name = _getNextSketchName();
        out.push_back(cur);
    }

    return out;
}

//writes the data as JSON
void Document::_writeNative(QTextStream &stream)
{
    stream << "[\n";

    for(int i = 0; i < (int)_sketches.size(); ++i)
    {
        if(i > 0)
            stream << " ,\n    ";

        stream << "{ ";

        //write out coordinates
        stream << "\"pts\" : [ ";

        const Cornu::VectorC<Vector2d> &pts = _sketches[i].pts->pts();
        for(int j = 0; j < pts.size(); ++j)
        {
            if(j > 0)
                stream << " , ";
            stream << pts[j][0] << ", " << pts[j][1];
        }

        stream << " ] ";

        //write out parameters
        for(int j = 0; j < (int)Cornu::Parameters::parameters().size(); ++j)
        {
            const Cornu::Parameters::Parameter &param = Cornu::Parameters::parameters()[j];
            stream << " ,\n      \"" << param.typeName.c_str() << "\" : ";
            stream << _sketches[i].params.get(param.type);
        }

        //write out algorithms
        for(int j = 0; j < Cornu::NUM_ALGORITHM_STAGES; ++j)
        {
            Cornu::AlgorithmBase *alg = Cornu::AlgorithmBase::get((Cornu::AlgorithmStage)j, _sketches[i].params.getAlgorithm(j));
            stream << " ,\n      \"" <<  alg->stageName().c_str() << "\" : ";
            stream << "\"" << alg->name().c_str() << "\"";
        }

        stream << " }";
    }

    stream << " ]";
}

void Document::save()
{
    if(_sketches.empty())
        return; //nothing to do

    QString fileName = QFileDialog::getSaveFileName(_view, "Save Sketch",
                        "",
                        "Cornucopia files (*.cnc);;Old format (*.pts)");

    if(fileName.isEmpty())
        return;

    bool cnc = fileName.toLower().endsWith(".cnc");
    if(!cnc && !fileName.toLower().endsWith(".pts"))
    {
        QMessageBox::critical(_view, "Error", QString("Unrecognized extension"));
        return;
    }

    QFile file(fileName);
    if(!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(_view, "Error", QString("Could not open file for write: ") + fileName);
        return;
    }

    if(cnc)
    {
        QTextStream out(&file);
        _writeNative(out);
    }
    else
    {
        QDataStream out(&file);
        _writePts(out, _sketches.back().pts);
    }
}

QString Document::_getNextSketchName()
{
    return QString("Sketch %1").arg(++_sketchIdx);
}

#include "Document.moc"